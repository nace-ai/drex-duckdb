#!/usr/bin/env bash
# Builds test/transport/transport_test.cpp against a build's static libraries (BUILD, default release: the directory
# under build/), starts the mock and the echo proxy, and runs it. Run from the repository root after `make`. The test
# calls DrexClient directly, so it covers the transport before prompt_drex and drex_decide are bound.
set -euo pipefail

ROOT=$(pwd)
export BUILD=${BUILD:-release}
OUT=$ROOT/build/transport-test-$BUILD
MOCK_PORT=${MOCK_PORT:-8787}
PROXY_PORT=${PROXY_PORT:-8788}
mkdir -p "$OUT"

# The include paths, defines and sanitizers the extension itself is compiled with
FLAGS=$(python3 - <<'PY'
import json, os, shlex
for entry in json.load(open(f"build/{os.environ['BUILD']}/compile_commands.json")):
    if entry["file"].endswith("src/drex_client.cpp") and "loadable" not in entry["command"]:
        prefixes = ("-I", "-D", "-std", "-fsanitize", "-fno-sanitize")
        flags = [a for a in shlex.split(entry["command"]) if a.startswith(prefixes)]
        print(" ".join(shlex.quote(f) for f in flags))
        break
PY
)

SANITIZE=$(tr ' ' '\n' <<<"$FLAGS" | grep sanitize | tr '\n' ' ' || true)

cd "build/$BUILD"
eval c++ -O1 "$FLAGS" -c "$ROOT/test/transport/transport_test.cpp" -o "$OUT/transport_test.o"
# The duckdb shell's link line, with the test in place of the shell
c++ $SANITIZE "$OUT/transport_test.o" \
    tools/shell/CMakeFiles/shell.dir/shell_static_extension_loader.c.o \
    tools/shell/CMakeFiles/shell.dir/__/__/extension/loader/static_extension_autoregister.cpp.o \
    -o "$OUT/transport_test" \
    extension/drex/libdrex_extension.a src/libduckdb_shell.a src/main/http/libduckdb_httplib.a \
    src/main/extension/libduckdb_loadable_extensions.a \
    src/main/extension/local_extension_repository/libduckdb_local_extension_repository.a \
    extension/core_functions/libcore_functions_extension.a extension/parquet/libparquet_extension.a \
    extension/json/libjson_extension.a extension/icu/libicu_extension.a third_party/utf8proc/libduckdb_utf8proc.a \
    src/libduckdb_static.a third_party/zstd/libduckdb_zstd.a
cd "$ROOT"

python3 test/mock/drex_mock.py --port "$MOCK_PORT" &
MOCK_PID=$!
python3 test/transport/echo_proxy.py "$PROXY_PORT" "$OUT/openrouter.pem" &
PROXY_PID=$!
trap 'kill $MOCK_PID $PROXY_PID 2>/dev/null' EXIT
sleep 1

DREX_MOCK_URL=http://127.0.0.1:$MOCK_PORT \
    HTTPFS_EXTENSION=$ROOT/build/$BUILD/extension/httpfs/httpfs.duckdb_extension \
    ECHO_PROXY_PORT=$PROXY_PORT \
    ECHO_PROXY_CA=$OUT/openrouter.pem \
    "$OUT/transport_test"
