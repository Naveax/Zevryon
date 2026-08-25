# M6 Linux PSI pressure-floor policy

This slice converts captured Linux memory PSI into pressure authority only when explicit calibrated thresholds are supplied.

## Fail-closed default

PSI pressure escalation is disabled by default. Disabled configuration requires all PSI thresholds to remain zero. Enabling PSI pressure authority without non-zero explicit thresholds is invalid.

This avoids silently promoting arbitrary laboratory thresholds into production policy.

## Explicit thresholds

An enabled policy supplies fixed-point milli-percent thresholds for:

- elevated `some avg10`,
- critical `some avg10`,
- critical `full avg10`.

All thresholds are bounded to 0..100000 milli-percent. The critical `some` threshold may not be lower than the elevated `some` threshold.

## Combination rule

The existing available-memory ratio and hysteresis policy remains authoritative. PSI contributes only a pressure floor:

- below all PSI thresholds: no additional pressure,
- elevated `some`: at least `Elevated`,
- critical `some` or critical `full`: at least `Critical`.

PSI can make pressure more conservative but cannot lower a pressure already required by effective available memory.

## Telemetry

Pressure statistics record the number of enabled PSI samples and the number of times PSI raised the result above the available-memory decision.

## Deliberate boundary

This slice does not choose production threshold values, does not add cgroup event notifications, and does not alter the default M5 behavior. Threshold calibration and event-driven wakeups remain separate evidence-backed work.
