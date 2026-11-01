#include "drex_secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

static constexpr const char *NACE_SECRET_TYPE = "nace";
static constexpr const char *DEFAULT_BASE_URL = "https://console.nace.ai";
static constexpr const char *API_KEY_ENV = "NACE_API_KEY";
static constexpr const char *BASE_URL_ENV = "NACE_BASE_URL";
static constexpr const char *OPENROUTER_API_KEY_ENV = "OPENROUTER_API_KEY";
static constexpr const char *NACE_KEY_PREFIX = "nace_";

//! Whether an http:// URL's host is this machine's localhost, 127.0.0.1 or [::1], compared whole
static bool IsLocalHttpUrl(const string &url) {
	// The authority ends at the path, the query or the fragment, and the host follows any user@ in it
	auto authority = url.substr(string("http://").size());
	authority = authority.substr(0, authority.find_first_of("/?#"));
	auto at = authority.rfind('@');
	if (at != string::npos) {
		authority = authority.substr(at + 1);
	}
	// The port follows the host, which keeps its brackets when it is an IPv6 address
	auto host_end = StringUtil::StartsWith(authority, "[") ? authority.find(']') + 1 : authority.find(':');
	auto host = StringUtil::Lower(authority.substr(0, host_end));
	if (host_end < authority.size() && authority[host_end] != ':') {
		return false;
	}
	return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

//! An http(s) URL without its trailing slashes; http:// only to this machine, since the key would cross the network
//! in the clear
static string NormalizeBaseUrl(const string &base_url) {
	auto result = base_url;
	while (StringUtil::EndsWith(result, "/")) {
		result.pop_back();
	}
	if (!StringUtil::StartsWith(result, "https://") && !StringUtil::StartsWith(result, "http://")) {
		throw InvalidInputException("nace secret: base_url must start with https:// or http://, got '%s'", base_url);
	}
	if (StringUtil::StartsWith(result, "http://") && !IsLocalHttpUrl(result)) {
		throw InvalidInputException("nace secret: base_url must use https:// outside localhost, got '%s'", base_url);
	}
	return result;
}

//! A Nace key is never sent to OpenRouter
static void RejectNaceKeyForOpenRouter(const string &api_key, const string &base_url) {
	if (IsOpenRouter(base_url) && StringUtil::StartsWith(api_key, NACE_KEY_PREFIX)) {
		throw InvalidInputException("nace secret: a %s key is never sent to OpenRouter; use an OpenRouter key, set "
		                            "%s or pass it as API_KEY",
		                            NACE_KEY_PREFIX, OPENROUTER_API_KEY_ENV);
	}
}

static string GetOption(const CreateSecretInput &input, const string &name) {
	auto entry = input.options.find(name);
	if (entry == input.options.end() || entry->second.IsNull()) {
		return string();
	}
	return entry->second.ToString();
}

//! An optional header value of the secret, which must stay on one line
static string GetHeaderOption(const CreateSecretInput &input, const string &name) {
	auto value = GetOption(input, name);
	if (value.find_first_of("\r\n") != string::npos) {
		throw InvalidInputException("nace secret: %s must not contain a line break", name);
	}
	return value;
}

static unique_ptr<BaseSecret> MakeNaceSecret(const CreateSecretInput &input, const string &api_key,
                                             const string &base_url) {
	// Credentials are looked up without a path, which an explicit scope would never match
	if (!input.scope.empty()) {
		throw InvalidInputException("nace secret: SCOPE is not supported");
	}
	if (api_key.empty()) {
		throw InvalidInputException("nace secret: api_key must not be empty");
	}
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	auto normalized_url = NormalizeBaseUrl(base_url);
	RejectNaceKeyForOpenRouter(api_key, normalized_url);
	secret->secret_map["api_key"] = Value(api_key);
	secret->secret_map["base_url"] = Value(normalized_url);
	for (auto name : {"referer", "title"}) {
		auto value = GetHeaderOption(input, name);
		if (!value.empty()) {
			secret->secret_map[name] = Value(value);
		}
	}
	secret->redact_keys = {"api_key"};
	return std::move(secret);
}

static unique_ptr<BaseSecret> CreateNaceSecretFromConfig(ClientContext &, CreateSecretInput &input) {
	auto api_key = GetOption(input, "api_key");
	if (api_key.empty()) {
		throw InvalidInputException("nace secret: the config provider requires API_KEY");
	}
	auto base_url = GetOption(input, "base_url");
	return MakeNaceSecret(input, api_key, base_url.empty() ? DEFAULT_BASE_URL : base_url);
}

//! Reads the environment when the secret is created, so later changes to it do not apply. On OpenRouter the key is
//! OPENROUTER_API_KEY, else NACE_API_KEY (an OpenRouter key kept there before OPENROUTER_API_KEY existed)
static unique_ptr<BaseSecret> CreateNaceSecretFromEnv(ClientContext &, CreateSecretInput &input) {
	auto base_url = FileSystem::GetEnvVariable(BASE_URL_ENV);
	base_url = NormalizeBaseUrl(base_url.empty() ? DEFAULT_BASE_URL : base_url);
	string api_key = IsOpenRouter(base_url) ? FileSystem::GetEnvVariable(OPENROUTER_API_KEY_ENV) : string();
	if (api_key.empty()) {
		api_key = FileSystem::GetEnvVariable(API_KEY_ENV);
	}
	if (api_key.empty()) {
		throw InvalidConfigurationException("nace secret: the env provider requires %s to be set%s", API_KEY_ENV,
		                                    IsOpenRouter(base_url) ? string(" (or ") + OPENROUTER_API_KEY_ENV + ")"
		                                                           : string());
	}
	return MakeNaceSecret(input, api_key, base_url);
}

void RegisterNaceSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = Identifier(NACE_SECRET_TYPE);
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "drex";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function = {NACE_SECRET_TYPE, "config", CreateNaceSecretFromConfig};
	config_function.named_parameters["api_key"] = LogicalType::VARCHAR;
	config_function.named_parameters["base_url"] = LogicalType::VARCHAR;
	config_function.named_parameters["referer"] = LogicalType::VARCHAR;
	config_function.named_parameters["title"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config_function);

	CreateSecretFunction env_function = {NACE_SECRET_TYPE, "env", CreateNaceSecretFromEnv};
	env_function.named_parameters["referer"] = LogicalType::VARCHAR;
	env_function.named_parameters["title"] = LogicalType::VARCHAR;
	loader.RegisterFunction(env_function);
}

static string OptionalValue(const KeyValueSecret &secret, const char *name) {
	auto value = secret.TryGetValue(name);
	return value.IsNull() ? string() : value.ToString();
}

DrexCredentials LookupDrexCredentials(ClientContext &context) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, string(), NACE_SECRET_TYPE);
	if (!match.HasMatch()) {
		throw InvalidConfigurationException(
		    "No nace secret found. Create one with CREATE SECRET (TYPE nace, API_KEY '...'), or with "
		    "CREATE SECRET (TYPE nace, PROVIDER env) to read %s",
		    API_KEY_ENV);
	}
	auto &secret = match.GetSecret().Cast<KeyValueSecret>();
	DrexCredentials credentials;
	credentials.api_key = secret.TryGetValue("api_key", true).ToString();
	credentials.base_url = secret.TryGetValue("base_url", true).ToString();
	// A persistent secret made before this guard, or edited on disk, is checked again
	RejectNaceKeyForOpenRouter(credentials.api_key, credentials.base_url);
	credentials.referer = OptionalValue(secret, "referer");
	credentials.title = OptionalValue(secret, "title");
	return credentials;
}

} // namespace duckdb
