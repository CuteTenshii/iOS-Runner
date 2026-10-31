# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.

iOSRunner is an Android app (arm64 only, `minSdk` 30) that runs iOS `.ipa` files without emulating a CPU. Like Wine, it loads the app's Mach-O binary and runs its arm64 code natively, with its own C++ implementations of the Apple libraries the app imports.

## Commands

```sh
./gradlew :app:assembleDebug                 # build the APK (needs NDK 27.1.12297006)
./gradlew :app:testDebugUnitTest             # JVM unit tests (only the template test so far)
./gradlew :app:testDebugUnitTest --tests 'moe.tenshii.iosrunner.ExampleUnitTest'
adb install -r -t app/build/outputs/apk/debug/app-debug.apk
```

`tools/host-test.sh <ipa>` builds the engine as a static arm64 binary and runs an app under `qemu-aarch64`, with no phone. The host build poisons guest allocations with `0x5b`, and imports the engine can't resolve are printed as `UNRESOLVED:` lines. It is the fastest way to check a change to the engine, and these environment variables extend it:

| Variable | Effect |
|---|---|
| `IOSRUNNER_TIMEOUT` | Seconds before the run is killed (default 30) |
| `IOSRUNNER_EFENCE=1` | Links `tools/efence.cpp`, a guard-page allocator that turns heap overruns and use-after-free into immediate faults |
| `IOSRUNNER_TRACE_HEAP=1` | Logs object destruction |
| `IOSRUNNER_SEED_ACCOUNTS=<file>` | Installs a file as Block Extreme Droite's saved `accounts.json` |

On a phone, the guest's log is at `cache/engine.log`, read with `adb shell run-as moe.tenshii.iosrunner cat cache/engine.log`. Installed apps are under `files/apps/<bundle id>/`. `logcat` can be empty on some phones, so fatal VM errors and uncaught C++ exceptions are also copied into this log.

## Architecture

**Kotlin host** (`app/src/main/java/`):
- `MainActivity` is the launcher, and `AppLibrary` and `AppleFormats` import IPAs and read plists and CgBI icons.
- `RunActivity` shows the status and log of a run.
- `GuestActivity` runs in its own `:engine` process. It draws the JSON view tree the engine emits using native Android views, hosts the `WebView`s behind `WKWebView`, and serves HTTP and cookies.

**Native engine** (`app/src/main/cpp/`):
- `macho.cpp` maps the binary and applies chained fixups. Each import is asked of `resolve_symbol` in `symbols.cpp`, which tries each shim's symbol table in turn.
- An import nobody implements gets a `PROT_NONE` guard address that remembers its name. Calling or reading it stops the run with `STOP: called unimplemented function` or `accessed unimplemented data`, so missing APIs show up by name. Some code also compares against these addresses to identify framework protocols and conformances, and many are harmless metadata references, so the "not implemented" count overstates what's missing.
- `engine.cpp` owns the guest thread, crash reports and the action queue.
- `swift_runtime.cpp` covers refcounting, witness tables and dispatch thunks. `type_system.cpp` covers metadata, the mangled-name parser and casts.
- `stdlib_shim`, `foundation_shim`, `concurrency_shim`, `objc_runtime`, `codable_shim`, `regex_shim`, `key_path` and `swiftui_shim` implement the libraries of the same name.
- `swiftui_shim.cpp` has resilient views build `ViewNode` boxes, walks the app's view values by reflection, and emits a JSON tree. `GuestActivity` sends actions back by id, and each action re-renders the tree.
- `concurrency_shim.cpp` runs every task on the one guest thread, from a ready queue plus sleep timers drained by the render loop.

### Writing shims

Shims are hand-written against the Swift ABI, so most bugs are ABI mismatches:
- Use `__attribute__((swiftcall))` with `swift_indirect_result` (x8), `swift_context` (x20) and `swift_error_result` (x21).
- **Read the call site in the guest's disassembly before writing a shim.** Don't derive registers from the signature alone: `self` of a loadable struct is often an ordinary argument, as with `Font.monospaced()` in `x0`, and generic parameters and witness tables come in non-obvious orders. Disassemble with `llvm-objdump --macho -d`.
- Types the app reaches through metadata need the layout its compiled code expects. `frozen_layouts()` in `type_system.cpp` lists these; any other framework value type defaults to one retained reference. Class metadata needs an instance size at `+0x30`, because guest code reads it before `swift_allocObject`.
- Heap corruption often appears later as an allocator abort on an unrelated thread. Reproduce it with `IOSRUNNER_EFENCE=1` instead of reasoning from the crash site.

## Testing against real services

Guest apps do what they were built to do, including reporting to their servers and acting on real accounts. Test risky flows with fixtures: `IOSRUNNER_SEED_ACCOUNTS` on the host, or a local page reached through `adb reverse` on a phone, tapping with `adb shell input tap`. Block Extreme Droite's `BlockCounter` posts every completed block to the live site, even when the block itself was against a fixture.
