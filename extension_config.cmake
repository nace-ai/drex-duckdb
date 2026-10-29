# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(drex
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# httpfs provides the HTTPS client that HTTPUtil sends POST requests through; it is built here so the tests can load
# it. The pin matches duckdb/.github/config/extensions/httpfs.cmake for the DuckDB version in ./duckdb. The tidy
# build only checks ./src and its CI job does not install curl, so it leaves httpfs out. So do the distribution builds
# (ours and the community extensions', which set DUCKDB_PLATFORM): their tests skip without the mock or a live key, and
# their runners have no usable curl and OpenSSL without vcpkg.
if(NOT CLANG_TIDY AND NOT DUCKDB_EXPLICIT_PLATFORM)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/duckdb/duckdb-httpfs
        GIT_TAG 77392a72077a85897db2519fae11e2c0da972b8a
    )
endif()
