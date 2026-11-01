#!/usr/bin/env bash
# Builds, installs and runs a guest app on the connected phone, then prints why it stopped, symbolized.
# usage: tools/phone-run.sh [options] <bundle id>
#   -n            use the installed build
#   -t SECONDS    how long to let it run before the first tap, and after each one (default 15)
#   --tap X,Y     tap these screen pixels; repeatable
#   --guard TEXT  link tools/efence.cpp, guarding allocations once a log line contains TEXT;
#                 an empty TEXT guards from launch, which only short runs have the memory mappings for
#   --keep        leave the app running afterwards
# The log and a screenshot are left in build/phone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PACKAGE=moe.tenshii.iosrunner
OUT=$ROOT/build/phone
build=1 seconds=15 keep=0 guarding=0 guard_text=
taps=()
while [[ ${1:-} == -* ]]; do
    case $1 in
        -n) build=0 ;;
        -t) seconds=$2; shift ;;
        --tap) taps+=("$2"); shift ;;
        --guard) guarding=1; guard_text=$2; shift ;;
        --keep) keep=1 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
app=${1:-}
if [[ -z $app ]]; then
    sed -n 3,9p "$0" >&2
    exit 2
fi
mkdir -p "$OUT"
cd "$ROOT"
if ((build)); then
    gradle_args=()
    if ((guarding)); then gradle_args=(-Piosrunner.efence=true "-Piosrunner.efenceAfter=$guard_text"); fi
    if ! ./gradlew :app:assembleDebug -q ${gradle_args[@]+"${gradle_args[@]}"} > "$OUT/gradle.log" 2>&1; then
        grep -nE 'error:|^e: |FAILED' "$OUT/gradle.log" | cut -c1-200
        exit 1
    fi
    adb install -r -t app/build/outputs/apk/debug/app-debug.apk | tail -n 1
fi
adb shell am force-stop "$PACKAGE"
adb shell run-as "$PACKAGE" rm -f cache/engine.log
adb shell am start -n "$PACKAGE/.MainActivity" --es run "$app" > /dev/null
adb shell sleep "$seconds"
for tap in ${taps[@]+"${taps[@]}"}; do
    adb shell input tap "${tap%,*}" "${tap#*,}"
    adb shell sleep "$seconds"
done
adb exec-out run-as "$PACKAGE" cat cache/engine.log > "$OUT/run.log"
adb exec-out screencap -p > "$OUT/screen.png"
alive=$(adb shell pidof "$PACKAGE:engine" || true)
if ((!keep)); then adb shell am force-stop "$PACKAGE"; fi
echo "alive: ${alive:-no}"
grep -nE '^(STOP|CRASH)' "$OUT/run.log" | cut -c1-200 || true
grep '^PRINT' "$OUT/run.log" | tail -n 3 | cut -c1-160 || true
grep '^HTTP' "$OUT/run.log" | sed 's/ ->.*//' | sort | uniq -c | sort -rn | head -n 8 | cut -c1-100 || true
crash=$(sed -n '/^CRASH/,$p' "$OUT/run.log")
if [[ -n $crash ]]; then
    grep -oE '#[0-9]+ [^ ]+' <<< "$crash" | head -n 16 | tr '\n' ' ' || true
    echo
    library=$(ls -t app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libiosrunner.so | head -n 1)
    addresses=$(grep -oE 'libiosrunner\.so\+0x[0-9a-f]+' <<< "$crash" | sed 's/.*+//' | head -n 12 || true)
    if [[ -n $addresses ]]; then
        # shellcheck disable=SC2086
        llvm-symbolizer --obj="$library" -f -C --no-inlines $addresses | grep -v '^$' | paste - - |
            sed "s|$ROOT/app/src/main/cpp/||" | grep -v 'ndk1::' | cut -c1-140 || true
    fi
fi
echo "screen: $OUT/screen.png"
