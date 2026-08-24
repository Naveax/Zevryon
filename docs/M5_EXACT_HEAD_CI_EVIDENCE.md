# M5 Exact-Head Windows/Linux CI Evidence

This slice makes the general Windows/Linux build jobs test the exact source head
rather than GitHub's synthetic pull-request merge commit.

## Checkout authority

Both `linux-build-test` and `windows-build-test` use:

```yaml
ref: ${{ github.event.pull_request.head.sha || github.sha }}
```

The same expression is passed to the evidence writer as the expected SHA.

## Evidence writer

`scripts/write_m5_ci_evidence.py` is standard-library-only and runs after the
full headless CTest suite. It:

1. validates the expected SHA format;
2. reads `git rev-parse HEAD`;
3. fails unless the checkout exactly matches the expected SHA;
4. reruns `python-frame-evidence-contract-smoke` as a focused CTest;
5. writes a JSON evidence artifact even on failure;
6. exits non-zero unless every check passes.

The artifact intentionally does not contain hostname, username, or other
machine-identifying data.

## Artifacts

Successful or failed evidence is uploaded as:

- `m5-ci-linux-<sha>`
- `m5-ci-windows-<sha>`

with payloads under `evidence/m5/ci/`.

The artifact certifies exact-head checkout and the Python evidence contract.
It does not claim physical frame-latency certification; that remains a separate
physical-device gate.
