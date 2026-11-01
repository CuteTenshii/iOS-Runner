# TODO

## Security

### Isolate guest apps from each other

Guest apps share iOSRunner's Android UID, so the Keychain's per-app separation only holds at the API level. A malicious guest could bypass it with raw syscalls or direct Keystore calls, and could also read other guests' files.

Plan: run each guest's engine in an `android:isolatedProcess` service, started with `bindIsolatedService`, which gives it its own random UID and almost no access. The main app acts as a broker over Binder and keeps each guest's files and Keychain key under the identity of the process it started.

- [ ] Prototype: load and run one guest app in an isolated service. SELinux may refuse to map guest code as executable there, so test this first.
- [ ] Move the UI and `hostQuery` traffic between `GuestActivity` and the engine across the process boundary.
- [ ] Route file access in `foundation_url` and `darwin_io` through the broker, handing back file descriptors.
- [ ] Route sockets and Keychain calls through the broker.
- [ ] Track Android permission grants per guest in the broker, since Android only sees iOSRunner.
- [ ] Decide what to do about guests that make their own syscalls, which will fail in the sandbox because the shims never see them.

Alternative: generate and sign one APK per guest. Each gets a permanent UID and its own Android permissions, but every import needs an install prompt, and every stub has to be updated when the engine changes.
