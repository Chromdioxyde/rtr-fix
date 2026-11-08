#!/bin/sh
# Builds build/dinput8.dll (x64 Windows DLL) with clang + lld against mingw-w64.
#
# Requirements (Arch Linux package names): clang lld mingw-w64-headers mingw-w64-crt
# Other distributions: any clang/lld plus the mingw-w64 headers and CRT, installed at
# /usr/x86_64-w64-mingw32 (override with MINGW_SYSROOT).
set -eu
cd "$(dirname "$0")"
SYSROOT="${MINGW_SYSROOT:-/usr/x86_64-w64-mingw32}"
mkdir -p build
clang --target=x86_64-w64-mingw32 --sysroot="$SYSROOT" -O2 \
    -ffreestanding -fno-builtin -Wall -Wextra \
    -shared -o build/dinput8.dll src/dinput8_proxy.c src/dinput8.def \
    -fuse-ld=lld -nostdlib -Wl,--entry=DllMain -lkernel32 -luser32
echo "built: $(pwd)/build/dinput8.dll"
