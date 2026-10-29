// Transport tests for DrexClient against test/mock/drex_mock.py (and echo_proxy.py for OpenRouter's shape).
// Run with test/transport/run.sh after `make`. Env: DREX_MOCK_URL, HTTPFS_EXTENSION (path to
// httpfs.duckdb_extension), ECHO_PROXY_PORT, ECHO_PROXY_CA (the certificate the proxy presents for openrouter.ai)
#include "duckdb.hpp"
#include "drex_client.hpp"
#include "drex_extension.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace duckdb;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::cerr << "  FAIL " << __LINE__ << ": " #cond << std::endl;                                             \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static const string KEY = "nace_sk_mockKeyForDrexContractTests_000000000000000";
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

//! As bind and execution call it: inside a transaction
static shared_ptr<DrexClient> GetClient(Connection &con) {
	shared_ptr<DrexClient> client;
	con.context->RunFunctionInTransaction([&]() { client = DrexClient::Get(*con.context); });
	return client;
}

static shared_ptr<DrexClient> Use(Connection &con, const string &ns, const string &scenario) {
	Q(con, "FROM read_text('" + mock + "/__reset/" + ns + "')");
	Q(con, "CREATE OR REPLACE SECRET drex (TYPE nace, API_KEY '" + KEY + "', BASE_URL '" + mock + "/ns/" + ns + "/" +
	           scenario + "')");
	return GetClient(con);
}

//! One value from the namespace's request log
static Value Log(Connection &con, const string &ns, const string &select) {
	auto result =
	    Q(con, "SELECT " + select + " FROM read_json('" + mock + "/__log/" + ns +
	               "', format = 'newline_delimited', columns = {seq: 'BIGINT', scenario: 'VARCHAR', "
	               "endpoint: 'VARCHAR', status: 'INTEGER', authorization_header: 'VARCHAR', user_agent: "
	               "'VARCHAR', x_nace_client: 'VARCHAR', idempotency_key: 'VARCHAR', content_type: 'VARCHAR', "
	               "model: 'VARCHAR', state_type: 'VARCHAR', state: 'VARCHAR', image_colours: 'VARCHAR[]', "
	               "body_sha256: 'VARCHAR', attempt: 'INTEGER', waited_ms: 'DOUBLE'}) WHERE endpoint <> "
	               "'(log)'");
	return First(std::move(result));
}

static string Fails(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		ErrorData error(e);
		std::cout << "    (" << error.Message() << ")" << std::endl;
		return error.RawMessage();
	}
	return "";
}

static double Seconds(std::chrono::steady_clock::time_point since) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

static vector<DrexRequest> Requests(const vector<string> &states, const string &questions) {
	vector<DrexRequest> result;
	for (auto &state : states) {
		result.push_back({state, questions, {}});
	}
	return result;
}

static const string REFUND =
    R"({"wants_refund": {"type": "noul", "instructions": "Is the customer asking for a refund?"}})";

int main() {
	mock = std::getenv("DREX_MOCK_URL");
	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
	DuckDB db(nullptr, &config);
	db.LoadStaticExtension<DrexExtension>();
	Connection con(db);
	Q(con, string("LOAD '") + std::getenv("HTTPFS_EXTENSION") + "'");
	Q(con, "SET enable_external_file_cache = false");

	std::cout << "ok: answers, order, headers, body as written" << std::endl;
	{
		auto client = Use(con, "t_ok", "ok");
		auto results = client->DecideAll("drex-v2.0",
		                                 Requests({R"("I was charged twice. Please refund one of the charges.")",
		                                           R"({"ticket": {"id": 7}})", R"("Second ticket")"},
		                                          REFUND),
		                                 optional_idx(), "prompt_drex");
		CHECK(results.size() == 3);
		for (auto &r : results) {
			CHECK(r.Success());
			CHECK(r.json.find("\"noul\":0.9868") != string::npos);
		}
		CHECK(Log(con, "t_ok", "count(*)") == Value::BIGINT(3));
		CHECK(Log(con, "t_ok", "bool_and(user_agent LIKE 'duckdb-drex/%' AND x_nace_client = user_agent)") ==
		      Value::BOOLEAN(true));
		CHECK(Log(con, "t_ok", "bool_and(authorization_header = 'Bearer " + KEY + "')") == Value::BOOLEAN(true));
		CHECK(Log(con, "t_ok", "bool_and(content_type = 'application/json')") == Value::BOOLEAN(true));
		CHECK(Log(con, "t_ok", "count(DISTINCT idempotency_key)") == Value::BIGINT(3));
		CHECK(Log(con, "t_ok", "bool_and(length(idempotency_key) = 64)") == Value::BOOLEAN(true));
		CHECK(Log(con, "t_ok", "list(state_type ORDER BY state_type)").ToString() == "[object, string, string]");
		CHECK(Log(con, "t_ok", "bool_and(model = 'drex-v2.0')") == Value::BOOLEAN(true));

		// Same body, same key, across calls
		client->Decide("drex-v2.0", R"("Second ticket")", REFUND, {}, "prompt_drex");
		CHECK(Log(con, "t_ok", "count(DISTINCT idempotency_key)") == Value::BIGINT(3));

		auto models = client->Models("prompt_drex");
		CHECK(models.Success() && models.json.find("\"drex-max\"") != string::npos);
		CHECK(Log(con, "t_ok", "count(*) FILTER (endpoint = '/v1/models')") == Value::BIGINT(1));
		CHECK(client->DecideAll("drex-v2.0", {}, optional_idx(), "prompt_drex").empty());
	}

	std::cout << "Get: one client per secret, replaced when it changes" << std::endl;
	{
		auto a = Use(con, "t_get", "ok");
		CHECK(GetClient(con) == a);
		auto b = Use(con, "t_get", "401");
		CHECK(b != a && b->Credentials().base_url == mock + "/ns/t_get/401");
		Q(con, "DROP SECRET drex");
		CHECK(Fails([&]() { GetClient(con); }).find("No nace secret found") != string::npos);
	}

	std::cout << "images: base64 in order" << std::endl;
	{
		auto client = Use(con, "t_img", "ok");
		auto red = First(Q(con, "SELECT content FROM read_blob('" + mock + "/__fixtures/red.png')"));
		auto blue = First(Q(con, "SELECT content FROM read_blob('" + mock + "/__fixtures/blue.png')"));
		vector<string> images {StringValue::Get(red), StringValue::Get(blue)};
		auto result = client->Decide(
		    "drex-max", R"("")",
		    R"({"c": {"type": "choice", "instructions": "Which colour is Image 2?", "criteria": {"red": null, "blue": null}}})",
		    images, "prompt_drex");
		CHECK(result.Success() && result.json.find("\"choice\":\"blue\"") != string::npos);
		CHECK(Log(con, "t_img", "image_colours[1] || image_colours[2]").ToString() == "redblue");

		// Over 4 MiB: the row is lost and nothing is sent
		auto big = client->Decide("drex-max", R"("")", REFUND, {StringValue::Get(red) + string(5 * 1024 * 1024, 'x')},
		                          "prompt_drex");
		CHECK(!big.Success() && big.error.find("4194304") != string::npos);
		CHECK(Log(con, "t_img", "count(*)") == Value::BIGINT(1));

		// Not a PNG, JPEG or WebP: that row is lost and never sent, the others are answered
		auto mixed = client->DecideAll("drex-max", {{R"("")", REFUND, {"GIF89a"}}, {R"("")", REFUND, images}},
		                               optional_idx(), "prompt_drex");
		CHECK(mixed[0].error == "image 1 is not a PNG, JPEG or WebP image");
		CHECK(mixed[1].Success());
		CHECK(Log(con, "t_img", "count(*)") == Value::BIGINT(2));
	}

	std::cout << "401, 402, 422 throw with the function's name; nothing more is sent" << std::endl;
	{
		auto client = Use(con, "t_401", "401");
		auto error = Fails([&]() { client->Decide("drex-v2.0", R"("x")", REFUND, {}, "prompt_drex"); });
		CHECK(error.rfind("prompt_drex: Invalid API key", 0) == 0);
		CHECK(error.find("mockKeyForDrexContractTests") == string::npos);
		CHECK(Fails([&]() { client->Models("prompt_drex"); }).rfind("prompt_drex: Invalid API key", 0) == 0);

		client = Use(con, "t_402", "402");
		error = Fails([&]() {
			client->DecideAll("drex-v2.0", Requests(vector<string>(20, R"("x")"), REFUND), optional_idx(1),
			                  "drex_decide");
		});
		CHECK(error.rfind("drex_decide: This account has insufficient available funds", 0) == 0);
		CHECK(error.find(mock + "/ns/t_402/402/dashboard/billing") != string::npos);
		CHECK(Log(con, "t_402", "count(*)") == Value::BIGINT(1));

		client = Use(con, "t_422", "422");
		error = Fails([&]() { client->Decide("drex-v2.0", R"("x")", REFUND, {}, "prompt_drex"); });
		CHECK(error.find("prompt_drex: Drex refused the request: questions.a.instructions: must be a string; "
		                 "questions.a.instructions: noul needs instructions or criteria") == 0);
		CHECK(Log(con, "t_422", "count(*)") == Value::BIGINT(1));

		CHECK(Fails([&]() {
			      client->Decide("drex-v2.0", "not json", REFUND, {}, "drex_decide");
		      }).rfind("drex_decide: the request's state is not valid JSON", 0) == 0);
	}

	std::cout << "429-once and 529-once: retried after retry-after-ms with the same body and key" << std::endl;
	{
		auto client = Use(con, "t_429", "429-once");
		auto result = client->Decide("drex-v2.0", R"("x")", REFUND, {}, "prompt_drex");
		CHECK(result.Success());
		CHECK(Log(con, "t_429", "list(status ORDER BY seq)").ToString() == "[429, 200]");
		CHECK(Log(con, "t_429", "count(DISTINCT idempotency_key) = 1 AND count(DISTINCT body_sha256) = 1") ==
		      Value::BOOLEAN(true));
		CHECK(Log(con, "t_429", "max(waited_ms) >= 1000") == Value::BOOLEAN(true));

		// concurrency 1: the second row goes while the first waits to retry, so the wait holds no slot
		client = Use(con, "t_529", "529-once");
		auto start = std::chrono::steady_clock::now();
		auto results = client->DecideAll("drex-v2.0", Requests({R"("first")", R"("second")"}, REFUND), optional_idx(1),
		                                 "prompt_drex");
		CHECK(results[0].Success() && results[1].Success());
		CHECK(Log(con, "t_529", "list(status ORDER BY seq)").ToString() == "[529, 529, 200, 200]");
		CHECK(Log(con, "t_529", "bool_and(attempt = 1 OR waited_ms >= 2000)") == Value::BOOLEAN(true));
		CHECK(Seconds(start) < 3.5);
	}

	std::cout << "529-always: 5 attempts, then the row is lost" << std::endl;
	{
		auto client = Use(con, "t_529a", "529-always");
		auto result = client->Decide("drex-v2.0", R"("x")", REFUND, {}, "prompt_drex");
		CHECK(!result.Success() && result.error.find("5 attempts") != string::npos);
		CHECK(Log(con, "t_529a", "list(attempt ORDER BY seq)").ToString() == "[1, 2, 3, 4, 5]");
		CHECK(Log(con, "t_529a", "count(DISTINCT idempotency_key)") == Value::BIGINT(1));
		CHECK(Log(con, "t_529a", "bool_and(attempt = 1 OR waited_ms >= 2000)") == Value::BOOLEAN(true));
	}

	std::cout << "drex_max_concurrency and drex_max_rpm bound the instance" << std::endl;
	{
		// 600 a minute holds a burst of 50 (5 s worth), which covers all 12
		Q(con, "SET drex_max_rpm = 600");
		auto client = Use(con, "t_rpm", "ok");
		auto start = std::chrono::steady_clock::now();
		auto results =
		    client->DecideAll("drex-v2.0", Requests(vector<string>(12, R"("x")"), REFUND), optional_idx(), "x");
		CHECK(Seconds(start) < 2);
		Q(con, "SET drex_max_rpm = 60");
		start = std::chrono::steady_clock::now();
		// 60 a minute holds 5 tokens, so the 6th and 7th wait about a second each
		results = client->DecideAll("drex-v2.0", Requests(vector<string>(7, R"("y")"), REFUND), optional_idx(), "x");
		CHECK(Seconds(start) >= 1.5);
		Q(con, "SET drex_max_rpm = 0");
		CHECK(Fails([&]() {
			      client->DecideAll("drex-v2.0", Requests({R"("x")"}, REFUND), optional_idx(0), "f");
		      }).rfind("f: concurrency must be at least 1", 0) == 0);
	}

	std::cout << "OpenRouter: headers and body shape" << std::endl;
	{
		auto red = First(Q(con, "SELECT content FROM read_blob('" + mock + "/__fixtures/red.png')"));
		Q(con, string("SET http_proxy = '127.0.0.1:") + std::getenv("ECHO_PROXY_PORT") + "'");
		Q(con, string("SET GLOBAL ca_cert_file = '") + std::getenv("ECHO_PROXY_CA") + "'");
		Q(con,
		  "CREATE OR REPLACE SECRET drex (TYPE nace, API_KEY 'sk-or-v1-test', BASE_URL 'https://openrouter.ai/api')");
		auto client = GetClient(con);
		auto result = client->Decide(
		    "drex-max", R"({"ticket": 1})",
		    R"({"a": {"type": "noul", "criteria": {"true": "yes", "false": "no"}}, "b": {"type": "noul", "instructions": null}, "c": {"type": "noul", "instructions": "Keep"}})",
		    {StringValue::Get(red)}, "prompt_drex");
		CHECK(result.Success());
		auto check = [&](const string &expr) {
			auto v = First(Q(con, "SELECT " + expr + " FROM (SELECT '" + result.json + "'::JSON AS j)"));
			return v.ToString();
		};
		CHECK(check("j->>'$.url'") == "https://openrouter.ai/api/v1/systemone");
		CHECK(check("j->>'$.headers.http-referer'") == "https://github.com/nace-ai/drex-duckdb");
		CHECK(check("j->>'$.headers.x-openrouter-title'") == "Drex for DuckDB");
		CHECK(check("j->>'$.headers.authorization'") == "Bearer sk-or-v1-test");
		CHECK(check("j->>'$.body.model'") == "nace-ai/drex-max");
		CHECK(check("j->>'$.body.state[0].type'") == "text");
		CHECK(check("j->>'$.body.state[0].text'") == R"({"ticket": 1})");
		CHECK(check("j->>'$.body.state[1].type'") == "image_url");
		CHECK(check("starts_with(j->>'$.body.state[1].image_url.url', 'data:image/png;base64,iVBOR')") == "true");
		CHECK(check("json_type(j, '$.body.images')") == "NULL");
		CHECK(check("j->>'$.body.questions.a.instructions'") == "");
		CHECK(check("j->>'$.body.questions.b.instructions'") == "");
		CHECK(check("j->>'$.body.questions.c.instructions'") == "Keep");
		CHECK(check("j->>'$.body.questions.a.criteria.true'") == "yes");

		// Text only: state as written
		result = client->Decide("drex-v2.0", R"("plain")", REFUND, {}, "prompt_drex");
		CHECK(check("j->>'$.body.state'") == "plain");
		CHECK(check("j->>'$.body.model'") == "nace-ai/drex-v2.0");
		Q(con, "RESET http_proxy");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) : string("all passed")) << std::endl;
	return failures ? 1 : 0;
}
