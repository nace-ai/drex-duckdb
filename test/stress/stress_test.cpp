// Stress tests for prompt_drex against test/mock/drex_mock.py: interrupts, secret rotation, closing the database
// mid-query, throughput and scale. Run with test/stress/run.sh [release|debug] after `make` or `make debug`. Env:
// DREX_MOCK_URL, HTTPFS_EXTENSION (path to httpfs.duckdb_extension), DREX_STRESS_ROWS (the scale test's rows, 0 to
// skip it)
#include "duckdb.hpp"
#include "drex_extension.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

using namespace duckdb;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::cerr << "  FAIL " << __LINE__ << ": " #cond << std::endl;                                             \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static const string KEY_A = "nace_sk_mockKeyForDrexContractTests_000000000000000";
static const string KEY_B = "nace_sk_mockKeyForDrexContractTests_111111111111111";
static string mock;

static unique_ptr<QueryResult> Q(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		std::cerr << "SQL failed: " << sql << "\n" << result->GetError() << std::endl;
		std::exit(2);
	}
	return result;
}

//! The first column of the first row
static Value First(unique_ptr<QueryResult> result) {
	for (auto &row : *result) {
		return row.GetBaseValue(0);
	}
	return Value();
}

static void Reset(Connection &con, const string &ns) {
	Q(con, "FROM read_text('" + mock + "/__reset/" + ns + "')");
}

static void Use(Connection &con, const string &ns, const string &scenario, const string &key = KEY_A) {
	Reset(con, ns);
	Q(con, "CREATE OR REPLACE SECRET drex (TYPE nace, API_KEY '" + key + "', BASE_URL '" + mock + "/ns/" + ns + "/" +
	           scenario + "')");
}

//! One value from the namespace's POST /v1/systemone requests
static Value Log(Connection &con, const string &ns, const string &select) {
	return First(Q(con, "SELECT " + select + " FROM read_json('" + mock + "/__log/" + ns +
	                        "', format = 'newline_delimited', columns = {endpoint: 'VARCHAR', authorization_header: "
	                        "'VARCHAR', state: 'VARCHAR', attempt: 'INTEGER', in_flight: 'INTEGER', received_at: "
	                        "'DOUBLE'}) WHERE endpoint = '/v1/systemone'"));
}

static idx_t Count(Connection &con, const string &ns, const string &where = "true") {
	return Log(con, ns, "count(*) FILTER (WHERE " + where + ")").GetValue<idx_t>();
}

//! Seconds since the epoch, as the mock's received_at
static double Now() {
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static double Seconds(Clock::time_point from, Clock::time_point to) {
	return std::chrono::duration<double>(to - from).count();
}

static void Sleep(double seconds) {
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

static idx_t ThreadCount() {
#ifdef __APPLE__
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return 0;
	}
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
	return count;
#else
	idx_t count = 0;
	if (auto dir = opendir("/proc/self/task")) {
		while (auto entry = readdir(dir)) {
			count += entry->d_name[0] != '.';
		}
		closedir(dir);
	}
	return count;
#endif
}

static idx_t ResidentBytes() {
#ifdef __APPLE__
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
	    KERN_SUCCESS) {
		return 0;
	}
	return info.resident_size;
#else
	long pages = 0, resident = 0;
	if (auto file = fopen("/proc/self/statm", "r")) {
		if (fscanf(file, "%ld %ld", &pages, &resident) != 2) {
			resident = 0;
		}
		fclose(file);
	}
	return idx_t(resident) * idx_t(sysconf(_SC_PAGESIZE));
#endif
}

static double MiB(idx_t bytes) {
	return double(bytes) / (1024 * 1024);
}

//! A query running on its own thread, so that the test can interrupt it or close the database under it
struct Running {
	Running(Connection &con, string sql) : started(Clock::now()) {
		thread = std::thread([this, &con, sql]() {
			auto result = con.Query(sql);
			if (result->HasError()) {
				error = result->GetError();
			} else {
				value = First(std::move(result));
			}
			finished = Clock::now();
		});
	}
	void Join() {
		thread.join();
	}

	Clock::time_point started;
	Clock::time_point finished;
	//! The query's error, else the first column of its first row
	string error;
	Value value;
	std::thread thread;
};

//! Interrupts `query` `delay` seconds after it started. The seconds from the interrupt to the query's end
static double InterruptAfter(Connection &con, Running &query, double delay, double &interrupted_at) {
	std::this_thread::sleep_until(query.started + std::chrono::duration<double>(delay));
	interrupted_at = Now();
	auto interrupted = Clock::now();
	con.Interrupt();
	query.Join();
	return Seconds(interrupted, query.finished);
}

static string Rows(idx_t count, const string &prefix = "Ticket") {
	return StringUtil::Format("SELECT count(prompt_drex('%s ' || i, 'Is the customer asking for a refund?')) FROM "
	                          "range(%llu) t(i)",
	                          prefix, count);
}

// A request on its way when the interrupt comes may reach the mock this long after it
static constexpr double SEND_SLACK = 0.05;

static void CheckInterrupted(Connection &con, const string &ns, Running &query, double stop, double interrupted_at,
                             idx_t threads) {
	std::cout << "    stopped " << stop << " s after the interrupt" << std::endl;
	CHECK(stop < 1.0);
	CHECK(StringUtil::Contains(query.error, "Interrupted"));
	CHECK(Count(con, ns, StringUtil::Format("received_at > %f", interrupted_at + SEND_SLACK)) == 0);
	// Nothing left running sends later
	auto sent = Count(con, ns);
	Sleep(1.5);
	CHECK(Count(con, ns) == sent);
	CHECK(ThreadCount() <= threads);
}

int main() {
	mock = std::getenv("DREX_MOCK_URL");
	const auto scale_rows = std::strtoull(std::getenv("DREX_STRESS_ROWS"), nullptr, 10);
	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
	const auto httpfs = string("LOAD '") + std::getenv("HTTPFS_EXTENSION") + "'";
	DuckDB db(nullptr, &config);
	db.LoadStaticExtension<DrexExtension>();
	Connection con(db);
	Connection other(db);
	Q(con, httpfs);
	Q(con, "SET enable_external_file_cache = false");
	Q(con, "SET drex_max_rpm = 0");
	Q(con, "SET drex_max_concurrency = 8");
	// The first call starts what lasts, so the thread count after it is the baseline. A thread httpfs started may exit
	// later, so every check is that the count is back to at most the baseline
	Use(con, "stress_warm_up", "ok");
	Q(con, Rows(16));
	const auto threads = ThreadCount();
	std::cout << "baseline: " << threads << " threads, " << MiB(ResidentBytes()) << " MiB resident" << std::endl;

	std::cout << "interrupt: while requests are in flight" << std::endl;
	{
		// 64 rows at 8 in flight, 500 ms each, take 4 s; the 8 in flight at the interrupt end within 500 ms
		Use(con, "stress_int_requests", "slow-500");
		Running query(con, Rows(64));
		double at;
		auto stop = InterruptAfter(con, query, 1.2, at);
		CheckInterrupted(con, "stress_int_requests", query, stop, at, threads);
		CHECK(Count(con, "stress_int_requests") < 64);
	}

	std::cout << "interrupt: while rows wait out retry-after-ms" << std::endl;
	{
		// Every attempt is refused with retry-after-ms 2000, so the 8 rows wait 2 s before each of their 4 retries
		Use(con, "stress_int_retry", "529-always");
		Running query(con, Rows(8));
		double at;
		auto stop = InterruptAfter(con, query, 0.5, at);
		CheckInterrupted(con, "stress_int_retry", query, stop, at, threads);
		CHECK(Count(con, "stress_int_retry") == 8);
	}

	std::cout << "interrupt: while rows wait for a drex_max_rpm token" << std::endl;
	{
		// 6 a minute: the bind's GET /v1/models takes the bucket's one token, and the next comes in 10 s
		Use(con, "stress_int_rpm", "ok");
		Q(con, "SET drex_max_rpm = 6");
		Running query(con, Rows(4));
		double at;
		auto stop = InterruptAfter(con, query, 0.5, at);
		CheckInterrupted(con, "stress_int_rpm", query, stop, at, threads);
		CHECK(Count(con, "stress_int_rpm") == 0);
		Q(con, "SET drex_max_rpm = 0");
	}

	std::cout << "interrupt: while waiting for a slot another connection holds" << std::endl;
	{
		// The other connection's 8 rows hold all 8 slots for 3 s
		Use(con, "stress_int_slot", "slow-3000");
		Running holder(other, Rows(8, "Holder"));
		Sleep(0.3);
		for (auto &sql : {Rows(4, "Waiter"),
		                  // A model the cached list lacks: the bind fetches GET /v1/models again, and waits for a slot
		                  string("SELECT prompt_drex('Waiter', 'Refund?', model := 'drex-v9')")}) {
			Running query(con, sql);
			double at;
			auto stop = InterruptAfter(con, query, 0.2, at);
			std::cout << "    stopped " << stop << " s after the interrupt" << std::endl;
			CHECK(stop < 1.0);
			CHECK(StringUtil::Contains(query.error, "Interrupted"));
		}
		holder.Join();
		CHECK(holder.error.empty());
		CHECK(Count(con, "stress_int_slot", "state LIKE '%Waiter%'") == 0);
		CHECK(Count(con, "stress_int_slot") == 8);
	}

	std::cout << "secret: replaced while a query runs" << std::endl;
	{
		// 2,100 rows are two chunks, so the second chunk looks the secret up after it was replaced
		Use(con, "stress_rotate_a", "slow-20");
		Reset(con, "stress_rotate_b");
		Q(con, "SET drex_max_concurrency = 16");
		Running query(con, Rows(2100));
		Sleep(0.5);
		Q(other, "CREATE OR REPLACE SECRET drex (TYPE nace, API_KEY '" + KEY_B + "', BASE_URL '" + mock +
		             "/ns/stress_rotate_b/ok')");
		query.Join();
		CHECK(query.error.empty() && query.value == Value::BIGINT(2100));
		// The running query keeps the secret it started with; the next one takes the new one
		CHECK(Count(con, "stress_rotate_a", "authorization_header = 'Bearer " + KEY_A + "'") == 2100);
		CHECK(Count(con, "stress_rotate_b") == 0);
		Q(con, Rows(1));
		CHECK(Count(con, "stress_rotate_b", "authorization_header = 'Bearer " + KEY_B + "'") == 1);
		Q(con, "SET drex_max_concurrency = 8");
	}

	std::cout << "secret: dropped while a query runs" << std::endl;
	{
		Use(con, "stress_drop", "slow-20");
		Q(con, "SET drex_max_concurrency = 16");
		Running query(con, Rows(2100));
		Sleep(0.5);
		Q(other, "DROP SECRET drex");
		query.Join();
		CHECK(query.error.empty());
		CHECK(Count(con, "stress_drop") == 2100);
		auto after = con.Query(Rows(1));
		CHECK(after->HasError() && StringUtil::Contains(after->GetError(), "No nace secret found"));
		Q(con, "SET drex_max_concurrency = 8");
	}

	std::cout << "close: the database closed while a query runs, then its last connection" << std::endl;
	for (auto interrupt : {false, true}) {
		const auto before = ThreadCount();
		{
			auto closing = make_uniq<DuckDB>(nullptr, &config);
			closing->LoadStaticExtension<DrexExtension>();
			auto closing_con = make_uniq<Connection>(*closing);
			Q(*closing_con, httpfs);
			Q(*closing_con, "SET drex_max_rpm = 0");
			Use(*closing_con, "stress_close", "slow-200");
			Running query(*closing_con, Rows(64));
			Sleep(0.5);
			// The instance lives on while a connection does
			closing.reset();
			if (interrupt) {
				closing_con->Interrupt();
			}
			query.Join();
			if (interrupt) {
				CHECK(StringUtil::Contains(query.error, "Interrupted"));
			} else {
				CHECK(query.error.empty() && query.value == Value::BIGINT(64));
			}
			// The last reference: the instance, its limiter and its client go here
			closing_con.reset();
		}
		CHECK(ThreadCount() <= before);
	}

	std::cout << "throughput: 1,024 rows of 50 ms at 16 in flight" << std::endl;
	{
		Use(con, "stress_throughput", "slow-50");
		Q(con, "SET drex_max_concurrency = 16");
		auto started = Clock::now();
		Q(con, Rows(1024));
		auto rate = 1024 / Seconds(started, Clock::now());
		// 16 in flight, 50 ms each: 320 rows a second at best
		std::cout << "    " << rate << " rows/s of 320" << std::endl;
		CHECK(rate >= 0.8 * 320);
		CHECK(Log(con, "stress_throughput", "max(in_flight)") == Value::INTEGER(16));
		CHECK(ThreadCount() <= threads);
		Q(con, "SET drex_max_concurrency = 8");
	}

	if (scale_rows > 0) {
		std::cout << "scale: " << scale_rows << " rows, twice" << std::endl;
		Use(con, "stress_scale", "ok");
		Q(con, "SET drex_max_concurrency = 16");
		const auto duckdb_threads = First(Q(con, "SELECT current_setting('threads')")).GetValue<idx_t>();
		std::atomic<bool> done {false};
		std::atomic<idx_t> peak_threads {0};
		std::atomic<idx_t> peak_resident {0};
		std::thread monitor([&]() {
			while (!done) {
				peak_threads = MaxValue<idx_t>(peak_threads, ThreadCount());
				peak_resident = MaxValue<idx_t>(peak_resident, ResidentBytes());
				Sleep(0.02);
			}
		});
		const auto sql = StringUtil::Format(
		    "SELECT count(answer), sum(hash(i, answer)) FROM (SELECT i, prompt_drex('Ticket ' || i, 'What is it "
		    "about?', choice := ['billing', 'technical', 'other']) AS answer FROM range(%llu) t(i))",
		    scale_rows);
		vector<Value> results;
		vector<idx_t> resident;
		for (idx_t run = 0; run < 2; run++) {
			auto started = Clock::now();
			auto result = Q(con, sql);
			auto elapsed = Seconds(started, Clock::now());
			auto row = result->Fetch();
			CHECK(row->GetValue(0, 0) == Value::BIGINT(int64_t(scale_rows)));
			results.push_back(row->GetValue(1, 0));
			resident.push_back(ResidentBytes());
			std::cout << "    run " << run + 1 << ": " << elapsed << " s, " << double(scale_rows) / elapsed
			          << " rows/s, " << MiB(resident.back()) << " MiB resident after" << std::endl;
			if (run == 0) {
				// The mock keeps every request's body
				Reset(con, "stress_scale");
			}
		}
		done = true;
		monitor.join();
		std::cout << "    peak " << peak_threads << " threads, " << MiB(peak_resident) << " MiB resident; "
		          << ThreadCount() << " threads after" << std::endl;
		CHECK(results[0] == results[1]);
		// A second run holds no more memory than the first left behind. Measured before reading the mock's log, since
		// reading 100k records holds memory of its own
		CHECK(resident[1] <= resident[0] + 16 * 1024 * 1024);
		CHECK(Log(con, "stress_scale", "max(in_flight)").GetValue<idx_t>() <= 16);
		Reset(con, "stress_scale");
		// Each of DuckDB's threads runs at most one call, and a call starts at most 15 threads of its own
		CHECK(peak_threads <= threads + duckdb_threads * 15);
		CHECK(ThreadCount() <= threads);
	}

	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all passed" << std::endl;
	return 0;
}
