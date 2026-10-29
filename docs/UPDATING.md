# Updating DuckDB compatibility

This extension uses DuckDB's internal C++ API. A new DuckDB version can require code
changes and a rebuild; extension binaries are version- and platform-specific.

1. Update the `duckdb` submodule to the intended release tag and `extension-ci-tools` to
   its matching release branch. Commit the resulting submodule revisions.
2. Update `duckdb_version`, `ci_tools_version`, and reusable workflow references in
   `.github/workflows/MainDistributionPipeline.yml` together.
3. Update the `httpfs` commit in `extension_config.cmake` to match the new DuckDB release's
   `.github/config/extensions/httpfs.cmake`.
4. Update version references in the README and Python test instructions.
5. Build in a fresh build directory and run the mock SQL, transport, stress, Python,
   formatting, and sanitizer checks described in [the test guide](../test/README.md).
   Check the distribution workflow on each supported platform before release.

Use [DuckDB release notes](https://github.com/duckdb/duckdb/releases) and changes to its
C++ headers to investigate API differences. Do not assume a successful compile proves
query semantics or request counts are unchanged.
