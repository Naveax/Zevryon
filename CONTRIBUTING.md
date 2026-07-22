# Contributing

## Required local gate

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace --all-targets
python scripts/run_quality_gate.py
```

Changes to document ordering, height geometry, selection, cold storage or search
must include an invariant test. Security-sensitive changes require a regression
test and must not introduce `unsafe` code into this crate.

## Design rules

1. Logical completeness must not depend on render residency.
2. Viewport movement must not scan every logical node.
3. Complete operations must not silently omit provider-backed content.
4. Search prefilters may produce false positives, never false negatives.
5. Persistent data is validated before entering the logical document.
