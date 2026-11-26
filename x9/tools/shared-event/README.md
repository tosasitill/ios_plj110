# Native SharedEvent acceptance probe

This is a device-side, standalone probe for original iOS 20H392 IOSurface.framework, not the X9 Metal frontend. Build from the repository root:

```sh
export DEVELOPER_DIR="$HOME/Downloads/Xcode.app/Contents/Developer"
out=x9/out/x9-native-shared-event-probe-20261126
mkdir -p "$out"
xcrun --sdk iphoneos clang -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" \
  -target arm64-apple-ios16.0 -fobjc-arc -fblocks -Wall -Wextra -Werror \
  -framework Foundation x9/tools/shared-event/native_probe.m -o "$out/native-probe"
```

The probe must be run only via an existing, separately authorized execution path on the device. The build does not install, stage, run, or authorize it. It exercises real framework creation, Mach-name reimport **within the same task**, a timed wait, callback target/queue and high bits, monotonic updates, duplicate/early callback absence within bounded observation windows, and sequential reclamation above the simultaneous event cap. It reports the first failing stage. It does not prove cross-task transfer, forced port backpressure, a kernel sleep was entered before signaling, a real Metal drawable worked, or AegirPoster stopped aborting.

For the full SharedEvent acceptance, separately require a trusted device trace with experimental selectors active, genuine shared-event import and completion across two tasks, and AegirPoster startup + dark-screen behavior without the previous abort cycle. Compare the actual new log to 430; neither host compilation nor this same-task probe alone is acceptance.

Stock 20H392 labels for adjacent selectors: 42 is `IOSurfaceClientSignalEvent` (four scalars: surface ID, one 64-bit scalar, Mach name, one 64-bit scalar; the stock handler resolves the surface and types the event before handing both off). 43 is `IOSurfaceClientInitDetachModeCode` / `IOSurfaceClientSetDetachModeCode`; 44 is `IOSurfaceClientSetOwnershipIdentity`. This probe intentionally makes no claims about those paths. Do not substitute selector 37 for 42, as this would ignore stock surface-owned behavior.

Static lookup: `rg -n '; =(42|43|44)$' /tmp/iosurface.dis` identifies the three selector call sites; the fixed kernelcache slice `python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a604f8 d4` shows the selector 42 handler. No Apple artifacts belong in git.

## Offline review of an r6 boot capture

Run `python3 -I x9/tools/shared-event/review_log.py <capture>` on one local boot log. It reports observed flags, matched entry/return lines and same-PID calls up to the first AegirPoster SIGABRT. Duplicate/orphan diagnostic records or a missing/ambiguous boot-args marker disable first-abort correlation. Counts after the first display-off boundary are not a proof of uninterrupted darkness. The summary intentionally gives no PASS, even for a log with zero observed aborts: check completeness, workload, capture interval and device/probe evidence separately. The tool does not touch the phone or output raw log lines.
