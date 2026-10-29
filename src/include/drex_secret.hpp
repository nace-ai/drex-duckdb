#pragma once

#include "drex_client.hpp"

namespace duckdb {

class ExtensionLoader;

//! Registers the `nace` secret type with its `config` and `env` providers
void RegisterNaceSecret(ExtensionLoader &loader);

//! The credentials of the `nace` secret that applies to the context
//! @throws InvalidConfigurationException if there is none
DrexCredentials LookupDrexCredentials(ClientContext &context);

} // namespace duckdb
