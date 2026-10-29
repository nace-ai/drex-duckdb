#include "drex_client.hpp"

#include "drex_secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "mbedtls_wrapper.hpp"
#include "yyjson.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <thread>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT
using Clock = std::chrono::steady_clock;

static constexpr uint64_t REQUEST_TIMEOUT_SECONDS = 60;
//! Attempts per request, all with the same body and Idempotency-Key
static constexpr idx_t MAX_ATTEMPTS = 5;
static constexpr int64_t MAX_RETRY_WAIT_MS = 30000;
//! From a request's first attempt; a retry that would start after it is not made
static constexpr int64_t REQUEST_BUDGET_MS = 120000;
//! Drex refuses a larger body with a 413
static constexpr idx_t MAX_BODY_BYTES = idx_t(4) * 1024 * 1024;
//! How often a wait checks whether its query was interrupted, since nothing notifies it
static constexpr auto INTERRUPT_POLL = std::chrono::milliseconds(50);

static constexpr const char *MAX_CONCURRENCY_SETTING = "drex_max_concurrency";
static constexpr idx_t DEFAULT_MAX_CONCURRENCY = 8;
static constexpr const char *MAX_RPM_SETTING = "drex_max_rpm";
//! No limit: Drex answers an account over its own limit with a 429 and retry-after-ms, which the retries wait out
static constexpr idx_t DEFAULT_MAX_RPM = 0;

static constexpr const char *OPENROUTER_REFERER = "https://github.com/nace-ai/drex-duckdb";
static constexpr const char *OPENROUTER_TITLE = "Drex for DuckDB";
static constexpr const char *OPENROUTER_BILLING_URL = "https://openrouter.ai/settings/credits";

//! duckdb-drex/<version>, sent as User-Agent and X-Nace-Client
static string ClientName() {
#ifdef EXT_VERSION_DREX
	string version = EXT_VERSION_DREX;
#else
	string version;
#endif
	return "duckdb-drex/" + (version.empty() ? string("dev") : version);
}

//! Whether the base URL is OpenRouter's (https://openrouter.ai/api), which takes a different body and headers
static bool IsOpenRouter(const string &base_url) {
	auto host = base_url.substr(base_url.find("://") + 3);
	host = StringUtil::Lower(host.substr(0, host.find_first_of("/:?#")));
	return host == "openrouter.ai" || StringUtil::EndsWith(host, ".openrouter.ai");
}

//===--------------------------------------------------------------------===//
// The limiter
//===--------------------------------------------------------------------===//

static void ThrowIfInterrupted(optional_ptr<ClientContext> context) {
	if (context && context->IsInterrupted()) {
		throw InterruptException();
	}
}

//! Sleeps until `at`, or throws once the context's query is interrupted
static void SleepUntil(Clock::time_point at, optional_ptr<ClientContext> context) {
	while (Clock::now() < at) {
		ThrowIfInterrupted(context);
		std::this_thread::sleep_until(MinValue<Clock::time_point>(at, Clock::now() + INTERRUPT_POLL));
	}
	ThrowIfInterrupted(context);
}

//! The in-flight slots and the requests-per-minute bucket shared by every client of a database instance
class DrexLimiter {
public:
	//! Waits until fewer than `limit` requests are in flight across the instance, then takes a slot. Throws once the
	//! context's query is interrupted
	void AcquireSlot(idx_t limit, optional_ptr<ClientContext> context) {
		unique_lock<mutex> guard(slot_lock);
		while (!slot_released.wait_for(guard, INTERRUPT_POLL, [&]() { return in_flight < limit; })) {
			ThrowIfInterrupted(context);
		}
		in_flight++;
	}

	void ReleaseSlot() {
		{
			lock_guard<mutex> guard(slot_lock);
			in_flight--;
		}
		slot_released.notify_all();
	}

	//! Waits for a token of a bucket that refills at `rpm` per minute and holds 5 seconds' worth. 0 is no limit.
	//! Throws once the context's query is interrupted
	void TakeToken(idx_t rpm, optional_ptr<ClientContext> context) {
		if (rpm == 0) {
			return;
		}
		const double capacity = MaxValue<double>(1, static_cast<double>(rpm) / 12);
		const double tokens_per_ms = static_cast<double>(rpm) / 60000;
		unique_lock<mutex> guard(bucket_lock);
		while (true) {
			auto now = Clock::now();
			if (!bucket_started) {
				tokens = capacity;
				bucket_started = true;
			} else {
				std::chrono::duration<double, std::milli> elapsed = now - refilled;
				tokens = MinValue(capacity, tokens + elapsed.count() * tokens_per_ms);
			}
			refilled = now;
			if (tokens >= 1) {
				tokens -= 1;
				return;
			}
			const std::chrono::duration<double, std::milli> due((1 - tokens) / tokens_per_ms);
			bucket_refilled.wait_for(guard, MinValue<std::chrono::duration<double, std::milli>>(due, INTERRUPT_POLL));
			ThrowIfInterrupted(context);
		}
	}

private:
	mutex slot_lock;
	std::condition_variable slot_released;
	idx_t in_flight = 0;

	mutex bucket_lock;
	//! Never notified: waiters wake when their token is due
	std::condition_variable bucket_refilled;
	bool bucket_started = false;
	double tokens = 0;
	Clock::time_point refilled;
};

//! Holds one of the limiter's slots for one attempt, so a request waiting to retry holds none
class DrexSlot {
public:
	DrexSlot(DrexLimiter &limiter, idx_t limit, idx_t rpm, optional_ptr<ClientContext> context) : limiter(limiter) {
		limiter.AcquireSlot(limit, context);
		try {
			limiter.TakeToken(rpm, context);
			// Nothing is sent for a query interrupted while this waited
			ThrowIfInterrupted(context);
		} catch (...) {
			limiter.ReleaseSlot();
			throw;
		}
	}
	~DrexSlot() {
		limiter.ReleaseSlot();
	}
	DrexSlot(const DrexSlot &) = delete;
	DrexSlot &operator=(const DrexSlot &) = delete;

private:
	DrexLimiter &limiter;
};

//! What the extension keeps per database instance: the limiter, and the client for the current secret
class DrexInstanceState : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "drex_instance_state";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	//! Never evicted, so the limiter stays shared
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	const shared_ptr<DrexLimiter> limiter = make_shared_ptr<DrexLimiter>();
	mutex client_lock;
	shared_ptr<DrexClient> client;
};

static shared_ptr<DrexInstanceState> GetInstanceState(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<DrexInstanceState>(DrexInstanceState::ObjectType());
}

static idx_t GetSetting(DatabaseInstance &db, const char *name, idx_t default_value) {
	Value value;
	if (!db.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return default_value;
	}
	return value.GetValue<idx_t>();
}

static void CheckMaxConcurrency(ClientContext &, SetScope, Value &parameter) {
	if (!parameter.IsNull() && parameter.GetValue<idx_t>() == 0) {
		throw InvalidInputException("%s must be at least 1", MAX_CONCURRENCY_SETTING);
	}
}

void RegisterDrexClientSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(MAX_CONCURRENCY_SETTING,
	                          "Most Drex requests in flight at once across the database, unless a call passes "
	                          "concurrency :=",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_MAX_CONCURRENCY), CheckMaxConcurrency,
	                          SetScope::GLOBAL);
	config.AddExtensionOption(MAX_RPM_SETTING,
	                          "Most Drex requests sent per minute across the database: 0 (the default) for no limit; "
	                          "Drex's 429s pace each account",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_MAX_RPM), nullptr, SetScope::GLOBAL);
}

//===--------------------------------------------------------------------===//
// Request bodies
//===--------------------------------------------------------------------===//

struct JsonDoc {
	explicit JsonDoc(yyjson_doc *doc) : doc(doc) {
	}
	~JsonDoc() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
	JsonDoc(JsonDoc &&other) noexcept : doc(other.doc) {
		other.doc = nullptr;
	}
	JsonDoc(const JsonDoc &) = delete;
	JsonDoc &operator=(const JsonDoc &) = delete;

	yyjson_doc *doc;
};

struct MutJsonDoc {
	MutJsonDoc() : doc(yyjson_mut_doc_new(nullptr)) {
		if (!doc) {
			throw std::bad_alloc();
		}
	}
	~MutJsonDoc() {
		yyjson_mut_doc_free(doc);
	}
	MutJsonDoc(const MutJsonDoc &) = delete;
	MutJsonDoc &operator=(const MutJsonDoc &) = delete;

	string Write(yyjson_mut_val *value) const {
		size_t length;
		auto text = yyjson_mut_val_write(value, YYJSON_WRITE_NOFLAG, &length);
		if (!text) {
			throw InternalException("drex: could not serialize a request body");
		}
		string result(text, length);
		free(text);
		return result;
	}

	yyjson_mut_doc *doc;
};

static JsonDoc ParseJson(const string &json, const char *what, const string &function_name) {
	JsonDoc result(yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG));
	if (!result.doc) {
		throw InvalidInputException("%s: the request's %s is not valid JSON", function_name, what);
	}
	return result;
}

static string JsonString(const string &value) {
	MutJsonDoc doc;
	return doc.Write(yyjson_mut_strncpy(doc.doc, value.c_str(), value.size()));
}

static string Base64(const string &bytes) {
	return Blob::ToBase64(string_t(bytes.data(), UnsafeNumericCast<uint32_t>(bytes.size())));
}

//! Drex's body: state and questions as written, the images as base64
static string DrexBody(const string &model, const string &state, const string &questions, const vector<string> &images,
                       const string &function_name) {
	// Checked, since they are spliced into the body as written
	ParseJson(state, "state", function_name);
	ParseJson(questions, "questions", function_name);
	string body = "{\"model\":" + JsonString(model) + ",\"state\":" + state + ",\"questions\":" + questions;
	if (!images.empty()) {
		body += ",\"images\":[";
		for (idx_t i = 0; i < images.size(); i++) {
			body += (i == 0 ? "\"" : ",\"") + Base64(images[i]) + "\"";
		}
		body += "]";
	}
	return body + "}";
}

static const char *ImageMimeType(const string &image) {
	auto data = image.data();
	if (image.size() >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
		return "image/png";
	}
	if (image.size() >= 3 && memcmp(data, "\xff\xd8\xff", 3) == 0) {
		return "image/jpeg";
	}
	if (image.size() >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
		return "image/webp";
	}
	return nullptr;
}

//! Drex takes any base64 as an image, so the magic bytes are checked here, before the row is sent. Why the row has
//! no request, else empty
static string CheckImages(const DrexRequest &request) {
	for (idx_t i = 0; i < request.images.size(); i++) {
		if (!ImageMimeType(request.images[i])) {
			return StringUtil::Format("image %llu is not a PNG, JPEG or WebP image", i + 1);
		}
	}
	return string();
}

//! OpenRouter's body: the model under nace-ai/, every question with instructions, and the images as data URLs in
//! state content parts after the state as text
static string OpenRouterBody(const string &model, const string &state, const string &questions,
                             const vector<string> &images, const string &function_name) {
	auto state_doc = ParseJson(state, "state", function_name);
	auto questions_doc = ParseJson(questions, "questions", function_name);
	MutJsonDoc doc;
	auto root = yyjson_mut_obj(doc.doc);
	auto routed_model = model.find('/') == string::npos ? "nace-ai/" + model : model;
	yyjson_mut_obj_add_strncpy(doc.doc, root, "model", routed_model.c_str(), routed_model.size());

	auto state_root = yyjson_doc_get_root(state_doc.doc);
	if (images.empty()) {
		yyjson_mut_obj_add_val(doc.doc, root, "state", yyjson_val_mut_copy(doc.doc, state_root));
	} else {
		auto parts = yyjson_mut_obj_add_arr(doc.doc, root, "state");
		auto text = yyjson_mut_arr_add_obj(doc.doc, parts);
		yyjson_mut_obj_add_str(doc.doc, text, "type", "text");
		if (yyjson_is_str(state_root)) {
			yyjson_mut_obj_add_strncpy(doc.doc, text, "text", yyjson_get_str(state_root), yyjson_get_len(state_root));
		} else {
			yyjson_mut_obj_add_strncpy(doc.doc, text, "text", state.c_str(), state.size());
		}
		for (idx_t i = 0; i < images.size(); i++) {
			auto mime_type = ImageMimeType(images[i]);
			if (!mime_type) {
				throw InternalException("%s: image %llu was sent unchecked", function_name, i + 1);
			}
			auto url = StringUtil::Format("data:%s;base64,%s", mime_type, Base64(images[i]));
			auto part = yyjson_mut_arr_add_obj(doc.doc, parts);
			yyjson_mut_obj_add_str(doc.doc, part, "type", "image_url");
			auto image_url = yyjson_mut_obj_add_obj(doc.doc, part, "image_url");
			yyjson_mut_obj_add_strncpy(doc.doc, image_url, "url", url.c_str(), url.size());
		}
	}

	// OpenRouter refuses a question without instructions, which Drex allows when it has criteria
	auto questions_copy = yyjson_val_mut_copy(doc.doc, yyjson_doc_get_root(questions_doc.doc));
	if (yyjson_mut_is_obj(questions_copy)) {
		yyjson_mut_obj_iter iter;
		yyjson_mut_obj_iter_init(questions_copy, &iter);
		while (auto key = yyjson_mut_obj_iter_next(&iter)) {
			auto question = yyjson_mut_obj_iter_get_val(key);
			if (!yyjson_mut_is_obj(question)) {
				continue;
			}
			auto instructions = yyjson_mut_obj_get(question, "instructions");
			if (!instructions || yyjson_mut_is_null(instructions)) {
				yyjson_mut_obj_remove_key(question, "instructions");
				yyjson_mut_obj_add_str(doc.doc, question, "instructions", "");
			}
		}
	}
	yyjson_mut_obj_add_val(doc.doc, root, "questions", questions_copy);
	return doc.Write(root);
}

//! The same for every attempt of a body. Drex's /v1/systemone ignores it and bills only a 200, so a retry is billed
//! twice only when a 200 was lost on its way back
static string IdempotencyKey(const string &model, const string &body) {
	duckdb_mbedtls::MbedTlsWrapper::SHA256State sha;
	sha.AddString(model);
	sha.AddString(string(1, '\0'));
	sha.AddString(body);
	char hex[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT];
	sha.FinishHex(hex);
	return string(hex, sizeof(hex));
}

//===--------------------------------------------------------------------===//
// Sending and answers
//===--------------------------------------------------------------------===//

//! One request, the same on every attempt
struct DrexCall {
	bool post = true;
	string url;
	string body;
	string idempotency_key;
};

struct DrexHttpResponse {
	//! 0 when no response arrived
	int status = 0;
	string body;
	//! Why no response arrived (a connection error or a timeout)
	string transport_error;
	//! The wait the response asked for before a retry
	optional_idx retry_after_ms;
};

static void EnsureHttpfs(DatabaseInstance &db, const string &function_name) {
	if (!db.ExtensionIsLoaded("httpfs") && !ExtensionHelper::TryAutoLoadExtension(db, "httpfs")) {
		throw InvalidConfigurationException(
		    "%s: needs the httpfs extension to call Drex, and could not load it. Run INSTALL httpfs; LOAD httpfs;",
		    function_name);
	}
}

//! A non-negative number of milliseconds (`scale` 1) or seconds (`scale` 1000), else invalid
static optional_idx ParseWaitMs(string text, double scale) {
	StringUtil::Trim(text);
	char *end = nullptr;
	auto number = std::strtod(text.c_str(), &end);
	if (text.empty() || end != text.c_str() + text.size() || !(number >= 0)) {
		return optional_idx();
	}
	return optional_idx(static_cast<idx_t>(MinValue<double>(number * scale, MAX_RETRY_WAIT_MS)));
}

using DrexSession = HTTPTransportManager::Session;

//! What a worker sends its requests through, so that a connection is kept and reused between them, not opened for
//! each (and left in TIME_WAIT, which runs out of local ports at a few hundred requests a second)
static unique_ptr<DrexSession> OpenSession(DatabaseInstance &db, const string &url) {
	auto session = make_uniq<DrexSession>(db.config.GetHTTPTransportManager().CreateSession(db, url));
	auto &params = session->Parameters();
	params.timeout = REQUEST_TIMEOUT_SECONDS;
	// Retries are ours, with the same Idempotency-Key
	params.retries = 0;
	return session;
}

static DrexHttpResponse SendOnce(DrexSession &session, const DrexCredentials &credentials, const DrexCall &call) {
	auto &params = session.Parameters();
	const auto client_name = ClientName();
	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + credentials.api_key);
	headers.Insert("Accept", "application/json");
	headers.Insert("User-Agent", client_name);
	headers.Insert("X-Nace-Client", client_name);
	if (IsOpenRouter(credentials.base_url)) {
		headers.Insert("HTTP-Referer", OPENROUTER_REFERER);
		headers.Insert("X-OpenRouter-Title", OPENROUTER_TITLE);
	}

	unique_ptr<HTTPResponse> response;
	if (call.post) {
		headers.Insert("Content-Type", "application/json");
		headers.Insert("Idempotency-Key", call.idempotency_key);
		PostRequestInfo request(call.url, headers, params, const_data_ptr_cast(call.body.data()), call.body.size());
		request.try_request = true;
		response = session.Request(request);
		if (response->body.empty()) {
			response->body = std::move(request.buffer_out);
		}
	} else {
		GetRequestInfo request(call.url, headers, params, nullptr, nullptr);
		request.try_request = true;
		response = session.Request(request);
	}

	DrexHttpResponse result;
	if (response->HasRequestError() || response->status == HTTPStatusCode::INVALID) {
		result.transport_error = response->GetError();
		return result;
	}
	result.status = static_cast<int>(response->status);
	result.body = std::move(response->body);
	if (response->HasHeader("retry-after-ms")) {
		result.retry_after_ms = ParseWaitMs(response->GetHeaderValue("retry-after-ms"), 1);
	}
	if (!result.retry_after_ms.IsValid() && response->HasHeader("retry-after")) {
		result.retry_after_ms = ParseWaitMs(response->GetHeaderValue("retry-after"), 1000);
	}
	return result;
}

struct DrexIssue {
	string path;
	string message;
};

//! Drex's error envelope: {"error": {"type", "message", "issues": [{"path", "message"}]}, "request_id"}
struct DrexError {
	string message;
	vector<DrexIssue> issues;
	string request_id;
};

static string JsonText(yyjson_val *value) {
	return yyjson_is_str(value) ? string(yyjson_get_str(value), yyjson_get_len(value)) : string();
}

static DrexError ParseError(const string &body) {
	DrexError result;
	JsonDoc doc(yyjson_read(body.c_str(), body.size(), YYJSON_READ_NOFLAG));
	auto root = doc.doc ? yyjson_doc_get_root(doc.doc) : nullptr;
	if (!yyjson_is_obj(root)) {
		return result;
	}
	result.request_id = JsonText(yyjson_obj_get(root, "request_id"));
	auto error = yyjson_obj_get(root, "error");
	if (yyjson_is_str(error)) {
		result.message = JsonText(error);
	}
	if (!yyjson_is_obj(error)) {
		return result;
	}
	result.message = JsonText(yyjson_obj_get(error, "message"));
	auto issues = yyjson_obj_get(error, "issues");
	size_t index, count;
	yyjson_val *issue;
	yyjson_arr_foreach(issues, index, count, issue) {
		result.issues.push_back({JsonText(yyjson_obj_get(issue, "path")), JsonText(yyjson_obj_get(issue, "message"))});
	}
	return result;
}

//! `path: message`, or the message alone for an issue with no path
static string IssueText(const string &path, const string &message) {
	return path.empty() ? message : path + ": " + message;
}

static bool IsUnder(const string &path, const string &field) {
	return path == field || StringUtil::StartsWith(path, field + ".");
}

//! Whether a 422 is about the row rather than the query's constants (questions, model): a request over the token
//! limit (Drex's documented `Request too long:`, with issue path `state` or ""), or issues only under `state` and
//! `images`
static bool RefusesRow(const DrexError &error) {
	if (StringUtil::StartsWith(error.message, "Request too long:")) {
		return true;
	}
	for (auto &issue : error.issues) {
		if (!IsUnder(issue.path, "state") && !IsUnder(issue.path, "images")) {
			return false;
		}
	}
	return !error.issues.empty();
}

//! A row refusal's issues without the row's content: a path under `state` names the row's own JSON keys, so only
//! `state` is kept
static string RowIssues(const DrexError &error) {
	vector<string> issues;
	for (auto &issue : error.issues) {
		issues.push_back(IssueText(IsUnder(issue.path, "state") ? "state" : issue.path, issue.message));
	}
	return StringUtil::Join(issues, "; ");
}

//! The response's own message, else its status
static string DescribeFailure(const DrexHttpResponse &response) {
	if (response.status == 0) {
		return response.transport_error.empty() ? "no response" : response.transport_error;
	}
	auto error = ParseError(response.body);
	auto description = StringUtil::Format("HTTP %d", response.status);
	// A 422 that reaches here refused only the row, and its message may quote the row
	auto reason = response.status == 422 ? RowIssues(error) : error.message;
	if (!reason.empty()) {
		description += ": " + reason;
	}
	if (!error.request_id.empty()) {
		description += " (request " + error.request_id + ")";
	}
	return description;
}

//! Whether no response arrived because TLS failed, in the handshake or on the certificate, which no retry fixes
static bool IsTlsFailure(const DrexHttpResponse &response) {
	if (response.status != 0) {
		return false;
	}
	// curl's messages for these name SSL, and httplib's name SSL or the certificate
	auto error = StringUtil::Lower(response.transport_error);
	return StringUtil::Contains(error, "ssl") || StringUtil::Contains(error, "certificate");
}

//! Throws for the answers that fail the query: 401, 402, a 422 that is not about the row alone, and a TLS failure
static void ThrowIfFatal(const DrexHttpResponse &response, const DrexCredentials &credentials,
                         const string &function_name) {
	if (IsTlsFailure(response)) {
		throw IOException("%s: could not reach Drex over TLS: %s", function_name, response.transport_error);
	}
	if (response.status != 401 && response.status != 402 && response.status != 422) {
		return;
	}
	auto error = ParseError(response.body);
	if (response.status == 422 && RefusesRow(error)) {
		return;
	}
	auto request = error.request_id.empty() ? string() : " (request " + error.request_id + ")";
	switch (response.status) {
	case 401:
		throw InvalidConfigurationException(
		    "%s: %s%s", function_name, error.message.empty() ? "the API key was refused (HTTP 401)" : error.message,
		    request);
	case 402: {
		auto billing_url = IsOpenRouter(credentials.base_url) ? string(OPENROUTER_BILLING_URL)
		                                                      : credentials.base_url + "/dashboard/billing";
		throw InvalidConfigurationException(
		    "%s: %s Billing: %s%s", function_name,
		    error.message.empty() ? "the account has no funds left (HTTP 402)." : error.message, billing_url, request);
	}
	default: {
		vector<string> texts;
		for (auto &issue : error.issues) {
			texts.push_back(IssueText(issue.path, issue.message));
		}
		auto issues = texts.empty() ? error.message : StringUtil::Join(texts, "; ");
		throw InvalidInputException("%s: Drex refused the request: %s%s", function_name,
		                            issues.empty() ? "HTTP 422" : issues, request);
	}
	}
}

static bool IsRetryable(const DrexHttpResponse &response) {
	switch (response.status) {
	case 0: // a connection error or a timeout
	case 408:
	case 429:
	case 500:
	case 502:
	case 503:
	case 504:
	case 529:
		return true;
	default:
		return false;
	}
}

//! The wait before retry number `retry` (1-based): what the response asked for, capped, else 1, 2, 4 and 8 s less up
//! to 25 % jitter
static int64_t RetryWaitMs(const DrexHttpResponse &response, idx_t retry) {
	if (response.retry_after_ms.IsValid()) {
		return static_cast<int64_t>(response.retry_after_ms.GetIndex());
	}
	thread_local std::mt19937_64 random(std::random_device {}());
	const int64_t backoff_ms = int64_t(1000) << MinValue<idx_t>(retry - 1, 4);
	std::uniform_int_distribution<int64_t> jitter(0, backoff_ms / 4);
	return MinValue(backoff_ms - jitter(random), MAX_RETRY_WAIT_MS);
}

//! What one attempt leaves a request with
struct DrexAttempt {
	enum class Outcome { ANSWERED, RETRY, LOST } outcome;
	string json;
	string error;
	Clock::time_point retry_at;
};

//! Sends attempt number `attempt` of a request first sent at `started`, and decides what follows
static DrexAttempt Attempt(DrexSession &session, DrexLimiter &limiter, const DrexCredentials &credentials,
                           const DrexCall &call, idx_t attempt, Clock::time_point started, idx_t limit, idx_t rpm,
                           const string &function_name, optional_ptr<ClientContext> context) {
	DrexHttpResponse response;
	{
		DrexSlot slot(limiter, limit, rpm, context);
		response = SendOnce(session, credentials, call);
	}
	DrexAttempt result;
	if (response.status >= 200 && response.status < 300) {
		result.outcome = DrexAttempt::Outcome::ANSWERED;
		result.json = std::move(response.body);
		return result;
	}
	ThrowIfFatal(response, credentials, function_name);
	if (IsRetryable(response) && attempt < MAX_ATTEMPTS) {
		result.retry_at = Clock::now() + std::chrono::milliseconds(RetryWaitMs(response, attempt));
		if (result.retry_at <= started + std::chrono::milliseconds(REQUEST_BUDGET_MS)) {
			result.outcome = DrexAttempt::Outcome::RETRY;
			return result;
		}
	}
	result.outcome = DrexAttempt::Outcome::LOST;
	result.error = IsRetryable(response) ? StringUtil::Format("no answer after %llu attempts, the last %s", attempt,
	                                                          DescribeFailure(response))
	                                     : DescribeFailure(response);
	return result;
}

//! Hands a call's requests to its workers: retries that are due first, then requests not yet sent, so a request
//! waiting to retry holds neither a slot nor a worker
class DrexSchedule {
public:
	DrexSchedule(idx_t count, optional_ptr<ClientContext> context) : count(count), context(context) {
	}

	//! The next request to work on, waiting for a retry to come due; false once every request is done or the call
	//! has failed, which an interrupt of the context's query fails
	bool Next(idx_t &index) {
		unique_lock<mutex> guard(lock);
		while (!failure) {
			if (context && context->IsInterrupted()) {
				failure = std::make_exception_ptr(InterruptException());
				return false;
			}
			if (!retries.empty() && retries.top().first <= Clock::now()) {
				index = retries.top().second;
				retries.pop();
				busy++;
				return true;
			}
			if (next < count) {
				index = next++;
				busy++;
				return true;
			}
			if (!retries.empty()) {
				// A copy: wait_until reads it after waking, when another worker may have moved the queue's storage
				const auto due = MinValue<Clock::time_point>(retries.top().first, Clock::now() + INTERRUPT_POLL);
				changed.wait_until(guard, due);
			} else if (busy > 0) {
				// A request being worked on may yet need a retry
				changed.wait_for(guard, INTERRUPT_POLL);
			} else {
				return false;
			}
		}
		return false;
	}

	void Finish() {
		Settle([]() {});
	}

	void Retry(idx_t index, Clock::time_point at) {
		Settle([&]() { retries.emplace(at, index); });
	}

	//! Stops the call: no request not yet started is sent
	void Fail(std::exception_ptr error) {
		Settle([&]() {
			if (!failure) {
				failure = std::move(error);
			}
		});
	}

	void ThrowIfFailed() {
		lock_guard<mutex> guard(lock);
		if (failure) {
			std::rethrow_exception(failure);
		}
	}

private:
	template <class F>
	void Settle(F &&update) {
		{
			lock_guard<mutex> guard(lock);
			busy--;
			update();
		}
		changed.notify_all();
	}

	using Due = std::pair<Clock::time_point, idx_t>;

	const idx_t count;
	const optional_ptr<ClientContext> context;
	mutex lock;
	std::condition_variable changed;
	idx_t next = 0;
	idx_t busy = 0;
	std::priority_queue<Due, vector<Due>, std::greater<Due>> retries;
	std::exception_ptr failure;
};

//===--------------------------------------------------------------------===//
// DrexClient
//===--------------------------------------------------------------------===//

DrexClient::DrexClient(DatabaseInstance &db, DrexCredentials credentials)
    : db(db), credentials(std::move(credentials)), limiter(GetInstanceState(db)->limiter) {
}

shared_ptr<DrexClient> DrexClient::Get(ClientContext &context) {
	auto credentials = LookupDrexCredentials(context);
	auto &db = DatabaseInstance::GetDatabase(context);
	auto state = GetInstanceState(db);
	lock_guard<mutex> guard(state->client_lock);
	auto &client = state->client;
	if (!client || client->credentials.api_key != credentials.api_key ||
	    client->credentials.base_url != credentials.base_url) {
		client = make_shared_ptr<DrexClient>(db, std::move(credentials));
	}
	return client;
}

DrexResult DrexClient::Decide(const string &model, const string &state, const string &questions,
                              const vector<string> &images, const string &function_name) const {
	vector<DrexRequest> requests(1);
	requests[0].state = state;
	requests[0].questions = questions;
	requests[0].images = images;
	return std::move(DecideAll(model, requests, optional_idx(), function_name)[0]);
}

vector<DrexResult> DrexClient::DecideAll(const string &model, const vector<DrexRequest> &requests,
                                         optional_idx concurrency, const string &function_name,
                                         optional_ptr<ClientContext> context) const {
	vector<DrexResult> results(requests.size());
	if (requests.empty()) {
		return results;
	}
	const auto limit = concurrency.IsValid() ? concurrency.GetIndex()
	                                         : GetSetting(db, MAX_CONCURRENCY_SETTING, DEFAULT_MAX_CONCURRENCY);
	if (limit == 0) {
		throw InvalidInputException("%s: concurrency must be at least 1", function_name);
	}
	const auto rpm = GetSetting(db, MAX_RPM_SETTING, DEFAULT_MAX_RPM);
	EnsureHttpfs(db, function_name);

	const auto open_router = IsOpenRouter(credentials.base_url);
	const auto url = credentials.base_url + "/v1/systemone";
	// Built on a request's first attempt and kept for its retries
	vector<DrexCall> calls(requests.size());
	vector<idx_t> attempts(requests.size(), 0);
	vector<Clock::time_point> started(requests.size());
	DrexSchedule schedule(requests.size(), context);

	auto work = [&]() {
		unique_ptr<DrexSession> session;
		idx_t index;
		while (schedule.Next(index)) {
			try {
				auto &call = calls[index];
				if (attempts[index] == 0) {
					auto &request = requests[index];
					results[index].error = CheckImages(request);
					if (!results[index].error.empty()) {
						schedule.Finish();
						continue;
					}
					call.url = url;
					call.body =
					    open_router
					        ? OpenRouterBody(model, request.state, request.questions, request.images, function_name)
					        : DrexBody(model, request.state, request.questions, request.images, function_name);
					if (call.body.size() > MAX_BODY_BYTES) {
						results[index].error = StringUtil::Format(
						    "the request body is %llu bytes, over Drex's limit of %llu; resize the images first",
						    call.body.size(), MAX_BODY_BYTES);
						schedule.Finish();
						continue;
					}
					call.idempotency_key = IdempotencyKey(model, call.body);
					started[index] = Clock::now();
				}
				if (!session) {
					session = OpenSession(db, url);
				}
				auto attempt = Attempt(*session, *limiter, credentials, call, ++attempts[index], started[index], limit,
				                       rpm, function_name, context);
				switch (attempt.outcome) {
				case DrexAttempt::Outcome::ANSWERED:
					results[index].json = std::move(attempt.json);
					schedule.Finish();
					break;
				case DrexAttempt::Outcome::RETRY:
					schedule.Retry(index, attempt.retry_at);
					break;
				case DrexAttempt::Outcome::LOST:
					results[index].error = std::move(attempt.error);
					schedule.Finish();
					break;
				}
			} catch (...) {
				schedule.Fail(std::current_exception());
			}
		}
	};

	// This thread is one of the workers
	const auto worker_count = MinValue<idx_t>(limit, requests.size());
	vector<std::thread> workers;
	try {
		for (idx_t i = 1; i < worker_count; i++) {
			workers.emplace_back(work);
		}
	} catch (...) {
		// Could not start a thread: the ones started still finish the call
	}
	work();
	for (auto &worker : workers) {
		worker.join();
	}
	schedule.ThrowIfFailed();
	return results;
}

DrexResult DrexClient::Models(const string &function_name, optional_ptr<ClientContext> context) const {
	EnsureHttpfs(db, function_name);
	const auto limit = GetSetting(db, MAX_CONCURRENCY_SETTING, DEFAULT_MAX_CONCURRENCY);
	const auto rpm = GetSetting(db, MAX_RPM_SETTING, DEFAULT_MAX_RPM);
	DrexCall call;
	call.post = false;
	call.url = credentials.base_url + "/v1/models";
	if (IsOpenRouter(credentials.base_url)) {
		// OpenRouter lists only text models unless asked for others
		call.url += "?output_modalities=decisions";
	}
	const auto started = Clock::now();
	auto session = OpenSession(db, call.url);
	DrexResult result;
	for (idx_t attempt = 1;; attempt++) {
		auto outcome =
		    Attempt(*session, *limiter, credentials, call, attempt, started, limit, rpm, function_name, context);
		switch (outcome.outcome) {
		case DrexAttempt::Outcome::ANSWERED:
			result.json = std::move(outcome.json);
			return result;
		case DrexAttempt::Outcome::LOST:
			result.error = std::move(outcome.error);
			return result;
		case DrexAttempt::Outcome::RETRY:
			SleepUntil(outcome.retry_at, context);
			break;
		}
	}
}

} // namespace duckdb
