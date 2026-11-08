#!/bin/sh
# Builds the DLL, runs the tests and creates two packages in dist/:
#
#   rtr-mousefix-<version>.zip         for users: dinput8.dll + ini + INSTALL.txt. Nothing to compile.
#   rtr-mousefix-<version>-source.zip  for developers: sources, docs, tests, build scripts, DLL.
#
# Set PROTON_FILES=<Proton>/files (or WINE=wine) to include the Wine integration test.
set -eu
cd "$(dirname "$0")"
VERSION="$(cat VERSION)"
USER_NAME="rtr-mousefix-$VERSION"
SRC_NAME="rtr-mousefix-$VERSION-source"

./build.sh
tests/run_tests.sh

rm -rf dist
mkdir -p "dist/$USER_NAME" "dist/$SRC_NAME/docs" "dist/$SRC_NAME/src" "dist/$SRC_NAME/tests"

# Example ini: extracted from the default text compiled into the DLL, so the two cannot drift apart.
python3 - <<'PY'
import re
src = open("src/dinput8_proxy.c").read()
block = src[src.index("k_ini_default[] ="):]
block = block[:block.index(";\n")]
text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', block)).replace("\\r\\n", "\n")
open("build/dinput8-proxy.ini", "w", newline="\n").write(text)
PY

# --- user package ---
cp build/dinput8.dll build/dinput8-proxy.ini INSTALL.txt CHANGELOG.md "dist/$USER_NAME/"
cp LICENSE "dist/$USER_NAME/LICENSE.txt"
(cd "dist/$USER_NAME" && sha256sum dinput8.dll > SHA256SUMS)

# --- source package ---
cp build/dinput8.dll build/dinput8-proxy.ini README.md INSTALL.txt LICENSE CHANGELOG.md VERSION build.sh release.sh "dist/$SRC_NAME/"
cp docs/ANALYSIS.md "dist/$SRC_NAME/docs/"
cp src/dinput8_proxy.c src/config.h src/dinput8.def "dist/$SRC_NAME/src/"
cp tests/test_config.c tests/wine_dinput_test.c tests/run_tests.sh tests/run_wine_test.sh "dist/$SRC_NAME/tests/"
(cd "dist/$SRC_NAME" && sha256sum dinput8.dll src/dinput8_proxy.c src/config.h > SHA256SUMS)

python3 - "$USER_NAME" "$SRC_NAME" <<'PY'
import os, sys, zipfile
for name in sys.argv[1:]:
    root = os.path.join("dist", name)
    with zipfile.ZipFile(f"dist/{name}.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for d, _, files in sorted(os.walk(root)):
            for f in sorted(files):
                p = os.path.join(d, f)
                info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, "dist"))
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(p, "rb") as fh:
                    z.writestr(info, fh.read())
PY
rm -rf "dist/$USER_NAME" "dist/$SRC_NAME"
echo "created:"
(cd dist && ls -l *.zip | awk '{print "  " $NF " (" $5 " bytes)"}' && sha256sum *.zip)
