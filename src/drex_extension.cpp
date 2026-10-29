#define DUCKDB_EXTENSION_MAIN

#include "drex_extension.hpp"
#include "drex_client.hpp"
#include "drex_functions.hpp"
#include "drex_secret.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterNaceSecret(loader);
	RegisterDrexClientSettings(loader);
	RegisterDrexFunctions(loader);
}

void DrexExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DrexExtension::Name() {
	return "drex";
}

std::string DrexExtension::Version() const {
#ifdef EXT_VERSION_DREX
	return EXT_VERSION_DREX;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(drex, loader) {
	duckdb::LoadInternal(loader);
}
}
