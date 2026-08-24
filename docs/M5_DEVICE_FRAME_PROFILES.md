# M5 — Device-Specific Frame Budgets

## Authority

Zevryon already defines four device classes in `zevryon_platform/performance_contract.py`:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

This slice makes the native M5 frame scheduler use the same names, RAM-selection thresholds, and scroll P99 targets rather than maintaining a generic 16.667 ms policy for every machine.

## Frame budgets

The native frame ceilings mirror the existing performance-contract scroll targets:

| Profile | Scroll / frame ceiling | Prefetch | Background | Maintenance |
| --- | ---: | ---: | ---: | ---: |
| legacy-phone | 33.300 ms | 4.000 ms | 1.000 ms | 0.500 ms |
| mid-phone | 16.600 ms | 2.000 ms | 0.500 ms | 0.250 ms |
| modern-phone | 11.100 ms | 1.250 ms | 0.300 ms | 0.150 ms |
| desktop | 8.330 ms | 1.000 ms | 0.250 ms | 0.125 ms |

Optional work remains subordinate to visible layout and to the scheduler's existing pressure scaling. Critical pressure still suppresses speculative work entirely.

## RAM class selection

The pure native selector mirrors the existing Python thresholds:

- `< 3072 MiB` → legacy-phone;
- `< 6144 MiB` → mid-phone;
- `< 12288 MiB` → modern-phone;
- otherwise → desktop.

This slice deliberately accepts measured RAM as input. Platform-specific physical-memory discovery and memory-pressure notification belong to M6, where Windows/Linux/Android backends are already planned.

## Runtime integration

`make_zenith_tab_runtime_config()` converts a selected native device profile into a valid `ZenithTabRuntimeConfig` while preserving the caller's `LayoutConfig`.

The browser shell can therefore perform:

1. obtain physical RAM from the platform backend;
2. select the device frame profile;
3. construct the tab runtime config through the profile factory;
4. create as many tab runtimes as resources permit without introducing a finite tab-count policy.

The profile changes frame-time admission only. It does not increase shared worker count, ready-result memory, or per-tab speculative queue cardinality.

## Validation

Focused tests certify:

- RAM threshold parity with the existing performance contract;
- exact frame ceilings for all four profiles;
- stable canonical profile names;
- validity of every generated scheduler policy;
- runtime factory propagation of frame budget, prefetch reserve, and caller layout policy.
