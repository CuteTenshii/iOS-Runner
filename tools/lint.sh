#!/usr/bin/env zsh
# Checks the native engine's formatting and runs clang-tidy against the NDK's headers. Needs both on PATH.
# usage: tools/lint.zsh [--fix]
ROOT=${0:A:h:h}
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=$(print -l $SDK/ndk/*(/on) | tail -1)
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
SRC=$ROOT/app/src/main/cpp
FILES=($SRC/*.cpp $SRC/*.h)
cd $ROOT
if [[ $1 == --fix ]]; then
    clang-format -i $FILES
    exit 0
fi
result=0
clang-format --dry-run --Werror $FILES || result=1
clang-tidy --quiet $SRC/*.cpp -- --target=aarch64-linux-android30 --sysroot=$BIN/../sysroot -std=c++20 -DIOSRUNNER_HOST || result=1
exit $result
