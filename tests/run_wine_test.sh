#!/bin/sh
# Runs tests/wine_dinput_test.c against build/dinput8.dll in a throw-away Wine prefix.
#
#   PROTON_FILES=/path/to/Proton/files  tests/run_wine_test.sh
#
# PROTON_FILES is the "files" directory of a Proton installation (it contains bin/wine).
# A plain Wine install works too: set WINE=/usr/bin/wine and leave PROTON_FILES unset.
# Needs an X display (DISPLAY) because the test creates a window and injects input.
#
# Verifies: (1) without RTR_DINPUT_FIX the 64-entry buffer overflows (hr=1) when 200 events
# queue up, (2) with RTR_DINPUT_FIX=1 the same input yields hr=0 and merged X/Y totals.
set -eu
cd "$(dirname "$0")/.."
DLL="$PWD/build/dinput8.dll"
[ -f "$DLL" ] || { echo "build/dinput8.dll missing: run ./build.sh first" >&2; exit 2; }

if [ -n "${PROTON_FILES:-}" ]; then
    WINE="$PROTON_FILES/bin/wine"
    export LD_LIBRARY_PATH="$PROTON_FILES/lib64:$PROTON_FILES/lib:${LD_LIBRARY_PATH:-}"
else
    WINE="${WINE:-wine}"
fi
SYSROOT="${MINGW_SYSROOT:-/usr/x86_64-w64-mingw32}"
WORK="$(mktemp -d)"
trap '"$WINE"server -k 2>/dev/null || true; rm -rf "$WORK"' EXIT
export WINEPREFIX="$WORK/pfx" WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml="

clang --target=x86_64-w64-mingw32 --sysroot="$SYSROOT" -O1 -ffreestanding -fno-builtin -nostdlib \
    -o "$WORK/test.exe" tests/wine_dinput_test.c -fuse-ld=lld -L"$SYSROOT/lib" \
    -Wl,--entry=mainstart -ldinput8 -ldxguid -luser32 -lkernel32

"$WINE" wineboot -u >/dev/null 2>&1
cp "$DLL" "$WINEPREFIX/drive_c/dinput8_proxy.dll"
cp "$WORK/test.exe" "$WINEPREFIX/drive_c/test.exe"

fail=0
run() { # $1 = fix flag
    PROXY_DLL='C:\dinput8_proxy.dll' RTR_DINPUT_FIX="$1" RTR_MOUSE_LINEAR=0 timeout 60 "$WINE" 'C:\test.exe' 2>&1 | tr -d '\r' | grep -E '^(round|CreateDevice|DirectInput8Create)'
}

echo "== RTR_DINPUT_FIX=0 (pass-through)"
out0="$(run 0)"; echo "$out0"
echo "$out0" | grep -q '^round 1: .* hr=00000001 ' || { echo "FAIL: expected DI_BUFFEROVERFLOW in round 1"; fail=1; }

echo "== RTR_DINPUT_FIX=1 (merge)"
# Real mouse movement during the test adds noise to the sums, so the check is retried up to 3 times.
# Do not touch the mouse while this runs.
attempt=1; ok1=0
while [ "$attempt" -le 3 ] && [ "$ok1" -eq 0 ]; do
    out1="$(run 1)"; echo "$out1"
    ok1=1
    for r in 1 2; do
        line="$(echo "$out1" | grep "^round $r:")"
        echo "$line" | grep -q ' hr=00000000 n=2 ' || ok1=0
        sx="$(echo "$line" | sed 's/.*sum_x=\(-\?[0-9]*\).*/\1/')"
        sy="$(echo "$line" | sed 's/.*sum_y=\(-\?[0-9]*\).*/\1/')"
        { [ "$sx" -ge 90 ] && [ "$sy" -ge 90 ]; } || ok1=0
    done
    [ "$ok1" -eq 1 ] || echo "(attempt $attempt did not meet the expectation, retrying)"
    attempt=$((attempt + 1))
done
[ "$ok1" -eq 1 ] || { echo "FAIL: merged mode should return DI_OK, n=2 and sums >= 90 in rounds 1 and 2"; fail=1; }

echo "== default settings in a foreign program (must not patch, must not crash)"
rm -f "$WINEPREFIX/drive_c/dinput8-proxy.log"
outd="$(PROXY_DLL='C:\dinput8_proxy.dll' timeout 60 "$WINE" 'C:\test.exe' 2>&1 | tr -d '\r' | grep -E '^(round 0|CreateDevice)' || true)"
echo "$outd"
echo "$outd" | grep -q '^round 0:' || { echo "FAIL: test program did not complete"; fail=1; }
grep -q 'call site does not match' "$WINEPREFIX/drive_c/dinput8-proxy.log" || { echo "FAIL: expected 'call site does not match' in the log"; fail=1; }

[ "$fail" -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
