# M0/M5 — Physical Benchmark Metadata and Thermal Evidence

## Purpose

Performance thresholds are not physical-device evidence by themselves. This slice adds a deterministic metadata envelope so a benchmark artifact records what class of machine produced it and whether thermal state was actually observed.

## Captured fields

`zevryon_platform.benchmark_metadata` captures:

- canonical `DeviceClass` selected by the existing RAM thresholds;
- physical RAM MiB and logical CPU count;
- OS name/release and architecture;
- bounded CPU model text;
- UTC capture time and optional bounded run label;
- explicit physical-device confirmation;
- thermal state and/or raw Celsius observations.

Hostname and username are intentionally not captured.

## Physical certification rule

CI, VM, container, or an arbitrary workstation must not silently qualify as a physical-device benchmark. Physical evidence requires:

`ZEVRYON_PHYSICAL_DEVICE=1`

and a thermal observation. Thermal evidence can come from Linux sysfs where available or from an explicit lab override:

- `ZEVRYON_THERMAL_STATE=nominal|fair|serious|critical|unknown`
- `ZEVRYON_THERMAL_C=51.5,52.0`

`physical_certification_checks()` fails closed when physical confirmation or thermal evidence is missing.

## Capture command

Use:

`python scripts/capture_benchmark_metadata.py --output evidence/m0/physical-device.json --require-physical`

The writer uses a temporary sibling file and replace operation so a partially written JSON file is not admitted as evidence.

## Canonical profile reuse

No second device taxonomy is introduced. Metadata calls `select_device_class()` from `performance_contract.py`, preserving the canonical profile thresholds already used by the performance contract and native M5 frame-budget profiles.

## Thermal semantics

The collector does not invent temperature-to-health thresholds. Raw Linux sensor values are evidence, while the qualitative state stays `unknown` unless an authoritative lab/OS source supplies it. This avoids pretending that one temperature threshold means the same thing on every CPU, phone SoC, and cooling design.

## Validation

Focused tests cover:

- explicit physical + thermal evidence passing;
- unconfirmed CI-like machines failing physical certification;
- canonical RAM-to-device-class selection;
- deterministic JSON;
- omission of hostname/username fields;
- invalid thermal state or malformed Celsius input failing closed.

These tests validate the evidence schema and collector behavior. They do not themselves certify any physical benchmark result.
