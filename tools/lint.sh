#!/usr/bin/env bash
# Checks the native engine's formatting and runs clang-tidy against the NDK's headers. Needs both on PATH.
# usage: tools/lint.sh [--fix]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=$(ls -d "$SDK"/ndk/* | sort -V | tail -n 1)
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
SRC=$ROOT/app/src/main/cpp
FILES=("$SRC"/*.cpp "$SRC"/*.h)
cd "$ROOT" || exit 1
if [[ ${1:-} == --fix ]]; then
    clang-format -i "${FILES[@]}"
    exit 0
fi
result=0
clang-format --dry-run --Werror "${FILES[@]}" || result=1
clang-tidy --quiet "$SRC"/*.cpp -- --target=aarch64-linux-android30 --sysroot="$BIN/../sysroot" -std=c++20 -DIOSRUNNER_HOST || result=1
exit $result
