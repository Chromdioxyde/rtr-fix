#!/bin/sh
# Host-side unit tests. Add PROTON_FILES=... (or WINE=...) to also run the Wine test.
set -eu
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
mkdir -p build
"$CC" -O1 -Wall -Wextra -o build/test_config tests/test_config.c -lm
build/test_config
if [ -n "${PROTON_FILES:-}${WINE:-}" ]; then
    [ -f build/dinput8.dll ] || ./build.sh
    sh tests/run_wine_test.sh
else
    echo "(skipping Wine test: set PROTON_FILES=<Proton>/files or WINE=wine to run it)"
fi
