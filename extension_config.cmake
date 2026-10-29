# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(drex
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# httpfs provides the HTTPS client that HTTPUtil sends POST requests through; it is built here so the tests can load
# it. The pin matches duckdb/.github/config/extensions/httpfs.cmake for the DuckDB version in ./duckdb. The tidy
# build only checks ./src and its CI job does not install curl, so it leaves httpfs out. So does a build without curl or
# OpenSSL, such as the community extensions' CI, whose tests need neither the mock nor the live API.
if(NOT CLANG_TIDY)
    find_package(OpenSSL QUIET)
    find_package(CURL QUIET)
    if(NOT OPENSSL_FOUND OR NOT CURL_FOUND)
        message(STATUS "Not building httpfs without OpenSSL and curl: the tests that call the mock or the live API skip")
    endif()
endif()
if(NOT CLANG_TIDY AND OPENSSL_FOUND AND CURL_FOUND)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/duckdb/duckdb-httpfs
        GIT_TAG 77392a72077a85897db2519fae11e2c0da972b8a
    )
endif()
