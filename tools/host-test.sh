#!/usr/bin/env zsh
# Builds the engine as a static arm64 binary and runs it under qemu-user, for testing without a phone.
# usage: tools/host-test.sh <path to .ipa>
set -e
ROOT=${0:A:h:h}
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=$(print -l $SDK/ndk/*(/on) | tail -1)
CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang++
OUT=$ROOT/build/host
mkdir -p $OUT
SRC=$ROOT/app/src/main/cpp
SOURCES=($SRC/*.cpp)
SOURCES=(${SOURCES:#*/jni_bridge.cpp})
EXTRA=()
if [[ -n $IOSRUNNER_EFENCE ]]; then
    EXTRA=($ROOT/tools/efence.cpp -Wl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=realloc,--wrap=posix_memalign,--wrap=malloc_usable_size)
fi
$CXX -std=c++20 -O1 -g -static -DIOSRUNNER_HOST ${IOSRUNNER_TRACE_HEAP:+-DIOSRUNNER_TRACE_HEAP} -Wall -Wextra -o $OUT/iosrunner-cli $SOURCES $EXTRA
BUNDLE=$OUT/bundle
rm -rf $BUNDLE && mkdir -p $BUNDLE
unzip -q -o "$1" 'Payload/*' -d $BUNDLE
APP=$(print -l $BUNDLE/Payload/*.app(/) | head -1)
EXE=${${APP:t}%.app}
# IOSRUNNER_SEED_ACCOUNTS: a file placed as the app's saved list, to test without the network.
if [[ -n $IOSRUNNER_SEED_ACCOUNTS ]]; then
    mkdir -p "$APP/.Library/Application Support"
    cp "$IOSRUNNER_SEED_ACCOUNTS" "$APP/.Library/Application Support/accounts.json"
fi
exec timeout ${IOSRUNNER_TIMEOUT:-30} qemu-aarch64 $OUT/iosrunner-cli "$APP" "$EXE"
