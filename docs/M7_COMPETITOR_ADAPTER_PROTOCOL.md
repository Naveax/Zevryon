# M7 competitor adapter protocol

## Purpose

M7 engine adapters use one vendor-independent subprocess protocol. The orchestrator sends one canonical request as JSON on stdin and accepts exactly one JSON object on stdout. Adapters do not write benchmark diagnostics to stdout because any extra bytes make the response invalid; stderr remains available for diagnostics.

## Request

Protocol identifier: `zevryon.m7.adapter.v1`.

The request binds:

- canonical engine identity;
- run index;
- corpus SHA-256 and logical byte count;
- local corpus path;
- canonical workload object plus `workload_sha256`;
- campaign system state.

The workload must use schema `zevryon.m7.workload.v1`, contain the corpus identity, a viewport object and a non-empty ordered operation array. Its canonical JSON SHA-256 must equal the request workload hash.

## Response

A valid response echoes protocol, engine, run index, corpus identity, workload identity and system state, then adds:

- exact engine version;
- UTC capture time;
- all nine canonical metrics for success, or an optional complete metric set plus `failure_mode` for a benchmark failure.

Identity rewrites are rejected. An adapter therefore cannot silently substitute another corpus, workload, run index, engine or machine context.

## Invocation failure

`invoke_adapter()` never uses a shell. It executes an argv sequence with `subprocess.run()` and a bounded timeout.

Timeout, launch failure, nonzero exit, malformed JSON, extra stdout, missing fields or identity mismatch are converted into a failed workload-bound raw run. The run retains the original corpus/workload/run/system identity and records the problem in `failure_mode`. It therefore remains visible to campaign aggregation and blocks leadership instead of disappearing from the evidence set.

## CLI

`scripts/run_m7_adapter.py` reads a canonical request JSON, invokes one adapter command and writes one schema-v2 raw run. It exits 0 for a successful benchmark response and 2 when the adapter invocation is preserved as a failed run.

## Test

`m7-competitor-adapter-protocol-smoke` covers valid execution, workload identity rewriting, noisy stdout, nonzero exit and timeout behavior using only the Python standard library.
