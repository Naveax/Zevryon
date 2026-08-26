# M6 Android trim-memory and low-RAM contract

The repository does not yet contain an Android Application/Activity/JNI lifecycle surface. This slice therefore defines the native policy contract that a future Android shell must call, without claiming that Java/Kotlin callbacks are already wired.

## Inputs

`ZenithAndroidMemorySignal` carries three Android platform facts:

- raw `ComponentCallbacks2.onTrimMemory(level)` value;
- `ActivityManager.isLowRamDevice()`;
- `ActivityManager.MemoryInfo.lowMemory`.

`isLowRamDevice()` selects the conservative `LegacyPhone` frame profile even when physical RAM alone would choose a larger profile. It is a device-class signal, not a transient pressure event.

## Pressure mapping

On Android API 34+, applications still receive `TRIM_MEMORY_UI_HIDDEN` and `TRIM_MEMORY_BACKGROUND`, but are no longer notified of the legacy `RUNNING_*`, `TRIM_MEMORY_MODERATE`, or `TRIM_MEMORY_COMPLETE` levels. The policy therefore relies on the two current lifecycle levels plus `MemoryInfo.lowMemory`; legacy values remain accepted for older Android shells.

The native mapping is:

- `MemoryInfo.lowMemory == true` -> `Critical`;
- trim level `>= TRIM_MEMORY_BACKGROUND (40)` -> `Critical` (also conservatively covers legacy `MODERATE`/`COMPLETE` values on older releases);
- `TRIM_MEMORY_UI_HIDDEN (20)..39` -> `Elevated`;
- legacy `TRIM_MEMORY_RUNNING_CRITICAL (15)..19` -> `Critical`;
- legacy running moderate/low values `5..14` -> `Elevated`;
- otherwise -> `Normal`.

The decision also reports `ui_hidden` and `background_lru` separately. A memory hint is not treated as proof that every browser tab is hidden; actual tab visibility remains lifecycle-owned state.

## Independent pressure sources

The process tab controller stores two independent pressure sources:

- `ProcessMemory`, updated by the existing process-memory sampler through `set_global_pressure()`;
- `PlatformMemory`, updated by Android through `set_pressure_source()`.

The controller's effective `global_pressure` is always the stronger of the two sources. A source update only reapplies tab policy when that effective pressure changes.

This prevents callback-order races. In particular:

1. Android may establish `Critical` platform pressure;
2. a later process-memory sample may recover to `Normal`;
3. the effective controller pressure remains `Critical` until Android clears its own platform source.

The reverse ordering is also safe: a normal Android callback cannot clear an independently active process-memory `Critical` source.

`apply_android_memory_pressure_signal()` evaluates the Android facts and updates only the `PlatformMemory` source. It does not receive or snapshot the process-memory policy, because one-time `max(process, android)` composition is insufficient when the two producers run at different times.

## Polling, threading and lifecycle boundary

No Android polling thread is added. A future Android shell should push trim and low-memory observations from the platform lifecycle while the existing process-memory sampler continues independently.

`ZenithProcessTabController` is owned by the existing process runtime/event-loop model and is not a cross-thread synchronization primitive. Android/JNI glue must therefore marshal platform memory callbacks onto the same owner/event-loop execution context used to mutate the process controller. It must not call `apply_android_memory_pressure_signal()` concurrently with process-memory sampling or tab lifecycle mutation from an arbitrary callback thread.

This owner-context rule is separate from pressure-source composition: independent `ProcessMemory` and `PlatformMemory` slots prevent logical clobbering, while event-loop serialization prevents native data races.

The future shell is also responsible for obtaining `isLowRamDevice()` and `MemoryInfo.lowMemory` from Android APIs and forwarding the raw facts into this native contract. JNI/Kotlin/Java glue is intentionally not fabricated in a repository that currently has no Android application shell.

## Validation

Focused regression tests cover:

- low-RAM device profile selection;
- `UI_HIDDEN`, background, legacy running and `lowMemory` mappings;
- invalid signal rejection;
- Android critical pressure surviving later process-memory recovery;
- process critical pressure surviving later Android normal/elevated callbacks;
- clearing one source without clearing the other;
- null bridge inputs failing closed.

Thread ownership itself is an integration responsibility of the future Android shell; these pure-native tests intentionally do not pretend to certify a JNI lifecycle that does not yet exist.
