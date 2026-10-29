#pragma once

#include "duckdb.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

class ExtensionLoader;
class DrexLimiter;

//! Where and as whom the client calls Drex, read from a `nace` secret
struct DrexCredentials {
	string api_key;
	//! Scheme and host without a trailing slash, e.g. https://console.nace.ai
	string base_url;
};

//! The outcome of one request: Drex's response body, or why there is none
struct DrexResult {
	//! The JSON body Drex answered with, when `error` is empty
	string json;
	//! Why the request has no answer (for example, retries ran out). The row it was made for returns NULL
	string error;

	bool Success() const {
		return error.empty();
	}
};

//! One row's request to POST /v1/systemone. The model and concurrency are per call, since a query keeps them constant
struct DrexRequest {
	//! The request's `state`, serialized as JSON (a JSON string for plain text)
	string state;
	//! The request's `questions` object, serialized as JSON
	string questions;
	//! Raw PNG, JPEG or WebP bytes, in order, which the questions refer to as Image 1, Image 2 and so on
	vector<string> images;
};

//! Calls the Drex API. Const methods are safe to call from several threads at once
class DrexClient {
public:
	DrexClient(DatabaseInstance &db, DrexCredentials credentials);

	//! The database instance's client for the `nace` secret that applies to the context. It is cached per instance, so
	//! every thread and query shares its in-flight limit, and replaced when the secret's key or base URL changes.
	//! Safe to call from several threads at once
	//! @throws InvalidConfigurationException if there is no `nace` secret
	static shared_ptr<DrexClient> Get(ClientContext &context);

	//! POST /v1/systemone for one row
	//! - `model`: the model id, e.g. drex-v2.0
	//! - `state`, `questions` and `images`: as in DrexRequest
	//! - `function_name`: the SQL function the request is for, which starts every error message
	//! Takes one slot of the instance's in-flight limit, like a request of DecideAll.
	//! Throws for answers that fail the query (401, 402, a 422 about the questions or model); returns an error for
	//! those that only lose the row
	DrexResult Decide(const string &model, const string &state, const string &questions, const vector<string> &images,
	                  const string &function_name = "drex") const;

	//! POST /v1/systemone for each of `requests`, concurrently, e.g. the non-NULL rows of a chunk
	//! - `model`: the model id, the same for every request
	//! - `concurrency`: at most this many requests in flight at once, else the `drex_max_concurrency` setting. Either
	//!   way they stay within the in-flight limit the instance's threads and queries share
	//! - `function_name`: as in Decide
	//! - `context`: the query's. Once it is interrupted, no request is sent and the call throws an InterruptException
	//!   as soon as the requests in flight end
	//! Returns one result per request, in the order of `requests`. Throws on the first answer that fails the query
	//! (401, 402, a 422 about the questions or model) and sends none of the requests not yet started. An answer that
	//! only loses its row is that row's error: running out of retries, or a 422 about the row (a state or request over
	//! the token limit, or an issue under `state` or `images`). So is a request never sent: a body over Drex's 4 MiB
	//! limit, or an image that is not a PNG, JPEG or WebP
	vector<DrexResult> DecideAll(const string &model, const vector<DrexRequest> &requests, optional_idx concurrency,
	                             const string &function_name = "drex",
	                             optional_ptr<ClientContext> context = nullptr) const;

	//! GET /v1/models (on OpenRouter, its decision models), with the same error semantics as Decide, and an interrupt
	//! of `context`'s query as in DecideAll
	DrexResult Models(const string &function_name = "drex", optional_ptr<ClientContext> context = nullptr) const;

	const DrexCredentials &Credentials() const {
		return credentials;
	}

private:
	DatabaseInstance &db;
	DrexCredentials credentials;
	//! The instance's, shared by all of its clients
	shared_ptr<DrexLimiter> limiter;
};

//! Registers the drex_max_concurrency and drex_max_rpm settings
void RegisterDrexClientSettings(ExtensionLoader &loader);

} // namespace duckdb
