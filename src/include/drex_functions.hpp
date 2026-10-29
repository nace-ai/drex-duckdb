#pragma once

namespace duckdb {

class ExtensionLoader;

//! Registers prompt_drex and drex_decide
void RegisterDrexFunctions(ExtensionLoader &loader);

} // namespace duckdb
