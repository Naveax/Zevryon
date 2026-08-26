# M6 Android trim-memory and low-RAM contract

The repository does not yet contain an Android Application/Activity/JNI lifecycle
surface. This slice therefore defines the native policy contract that a future
Android shell must call, without pretending that a Java/Kotlin callback is
already wired.

## Inputs

`ZenithAndroidMemorySignal` carries three platform facts:

- raw `ComponentCallbacks2.onTrimMemory(level)` value;
- `ActivityManager.isLowRamDevice()`;
- `ActivityManager.MemoryInfo.lowMemory`.

`isLowRamDevice()` selects the conservative `LegacyPhone` frame profile even if
physical RAM alone would otherwise choose a larger profile. It is a device
classification signal, not a transient pressure event.

## Pressure mapping

Modern Android no longer delivers the deprecated RUNNING_* trim levels to apps
on current platform releases, so the policy does not depend on them. They remain
accepted for older platform shells.

The native mapping is:

- `MemoryInfo.lowMemory == true` -> `Critical`
- `trim level >= TRIM_MEMORY_BACKGROUND (40)` -> `Critical`
- `TRIM_MEMORY_UI_HIDDEN (20)..39` -> `Elevated`
- legacy `TRIM_MEMORY_RUNNING_CRITICAL (15)..19` -> `Critical`
- legacy `TRIM_MEMORY_RUNNING_MODERATE/LOW (5)..14` -> `Elevated`
- otherwise -> `Normal`

The decision also reports `ui_hidden` and `background_lru` separately. Android
lifecycle code must still update actual tab visibility independently; a memory
hint is not treated as proof that every browser tab is hidden.

## Process integration

`apply_android_memory_pressure_signal()` evaluates the Android signal and then
applies the stronger of:

- the current `ZenithProcessMemoryPressurePolicy` pressure; and
- the Android pressure decision.

This composition is required because the process controller stores one global
pressure value. A normal Android callback must not lower an independently
established process-memory `Critical` state. Conversely, when the process policy
has recovered to `Normal`, clearing the Android signal may return the controller
to that normal baseline.

The bridge rejects a missing or invalid process-memory policy instead of guessing
that the baseline is `Normal`.

No Android polling thread is added. The future platform shell should push these
signals from its lifecycle callbacks and memory-state observations while the
existing process-memory policy remains the baseline authority.

## Current integration boundary

This commit contains the portable native policy, pressure-source composition,
controller bridge and focused regression tests. A real Android Java/Kotlin/JNI
callback remains an explicit integration task because no Android shell currently
exists in this repository.
