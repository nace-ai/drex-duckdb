#!/usr/bin/env bash
# Builds test/stress/stress_test.cpp against a build's static libraries, starts the mock on a free port, and runs it.
# Run from the repository root after `make` (release, the default) or `make debug` (debug: AddressSanitizer and
# UBSan, whose flags the test is compiled and linked with too). DREX_STRESS_ROWS sets the scale test's rows (default
# 100000 on release and 10000 on debug; 0 skips it).
set -euo pipefail

BUILD=${1:-release}
ROOT=$(pwd)
OUT=$ROOT/build/stress-test-$BUILD
mkdir -p "$OUT"

# The include paths, defines and sanitizers the extension itself is compiled with
FLAGS=$(BUILD=$BUILD python3 - <<'PY'
import json, os, shlex
for entry in json.load(open(f"build/{os.environ['BUILD']}/compile_commands.json")):
    if entry["file"].endswith("src/drex_client.cpp") and "loadable" not in entry["command"]:
        flags = [a for a in shlex.split(entry["command"]) if a.startswith(("-I", "-D", "-std", "-fsanitize", "-fno-sanitize"))]
        print(" ".join(shlex.quote(f) for f in flags))
        break
PY
)
SANITIZERS=$(printf '%s\n' $FLAGS | grep -e '^-fsanitize' -e '^-fno-sanitize' | tr '\n' ' ' || true)

cd "build/$BUILD"
eval c++ -O1 -g "$FLAGS" -c "$ROOT/test/stress/stress_test.cpp" -o "$OUT/stress_test.o"
# The duckdb shell's link line, with the test in place of the shell
c++ $SANITIZERS "$OUT/stress_test.o" \
    tools/shell/CMakeFiles/shell.dir/shell_static_extension_loader.c.o \
    tools/shell/CMakeFiles/shell.dir/__/__/extension/loader/static_extension_autoregister.cpp.o \
    -o "$OUT/stress_test" \
    extension/drex/libdrex_extension.a src/libduckdb_shell.a src/main/http/libduckdb_httplib.a \
    src/main/extension/libduckdb_loadable_extensions.a \
    src/main/extension/local_extension_repository/libduckdb_local_extension_repository.a \
    extension/core_functions/libcore_functions_extension.a extension/parquet/libparquet_extension.a \
    extension/json/libjson_extension.a extension/icu/libicu_extension.a third_party/utf8proc/libduckdb_utf8proc.a \
    src/libduckdb_static.a third_party/zstd/libduckdb_zstd.a
cd "$ROOT"

rm -f "$OUT/mock-url"
python3 test/mock/drex_mock.py --port 0 --url-file "$OUT/mock-url" &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null' EXIT
for _ in $(seq 50); do
    [ -s "$OUT/mock-url" ] && break
    sleep 0.1
done

if [ "$BUILD" = debug ]; then
    DEFAULT_ROWS=10000
else
    DEFAULT_ROWS=100000
fi
DREX_MOCK_URL=$(cat "$OUT/mock-url") \
    HTTPFS_EXTENSION=$ROOT/build/$BUILD/extension/httpfs/httpfs.duckdb_extension \
    DREX_STRESS_ROWS=${DREX_STRESS_ROWS:-$DEFAULT_ROWS} \
    "$OUT/stress_test"
