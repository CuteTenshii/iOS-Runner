#!/usr/bin/env bash
# Builds the engine as a static arm64 binary and runs it under qemu-user, for testing without a phone.
# usage: tools/host-test.sh <path to .ipa>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=$(ls -d "$SDK"/ndk/* | sort -V | tail -n 1)
CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang++
OUT=$ROOT/build/host
mkdir -p "$OUT"
SRC=$ROOT/app/src/main/cpp
SOURCES=()
for source in "$SRC"/*.cpp; do
    [[ $source == */jni_bridge.cpp ]] || SOURCES+=("$source")
done
EXTRA=()
if [[ -n ${IOSRUNNER_EFENCE:-} ]]; then
    EXTRA=("$ROOT/tools/efence.cpp" -Wl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=realloc,--wrap=posix_memalign,--wrap=malloc_usable_size)
fi
TRACE=()
if [[ -n ${IOSRUNNER_TRACE_HEAP:-} ]]; then TRACE=(-DIOSRUNNER_TRACE_HEAP); fi
"$CXX" -std=c++20 -O1 -g -static -DIOSRUNNER_HOST ${TRACE[@]+"${TRACE[@]}"} -Wall -Wextra -o "$OUT/iosrunner-cli" "${SOURCES[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
BUNDLE=$OUT/bundle
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
unzip -q -o "$1" 'Payload/*' -d "$BUNDLE"
APP=$(ls -d "$BUNDLE"/Payload/*.app | head -n 1)
EXE=$(basename "$APP" .app)
# IOSRUNNER_SEED_ACCOUNTS: a file placed as the app's saved list, to test without the network.
if [[ -n ${IOSRUNNER_SEED_ACCOUNTS:-} ]]; then
    mkdir -p "$APP/.Library/Application Support"
    cp "$IOSRUNNER_SEED_ACCOUNTS" "$APP/.Library/Application Support/accounts.json"
fi
exec timeout "${IOSRUNNER_TIMEOUT:-30}" qemu-aarch64 "$OUT/iosrunner-cli" "$APP" "$EXE"
