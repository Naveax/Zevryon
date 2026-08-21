# M7 Zevryon readiness adapter

## Scope

`scripts/m7_zevryon_adapter.py` is the concrete Zevryon implementation of the vendor-independent M7 adapter protocol. It remains fail-closed: a successful competitor raw-run is emitted only after all nine canonical metrics have real, semantically matching measurement primitives.

## Admitted primitives

Four metrics now have native measurement paths.

### Preindexed first viewport

`zevryon-m7-native-probe preindexed` measures the explicit boundary `open-plus-first-layout-v1`:

1. start the monotonic timer;
2. open `ZenithTabRuntime` on the prepared store;
3. apply visible/normal activity;
4. execute the first viewport layout;
5. stop only after a checkpoint-backed non-empty fragment set is available.

The adapter verifies the operation name, boundary token, canonical RAM-selected device profile, checkpoint use and positive fragment count before admitting the duration as `first_viewport_preindexed_ms`.

### Scroll P99 and maximum normal stall

The adapter reuses `zevryon-zenith-frame-probe`. Canonical workload sample count, warmup, viewport, overscan, maximum fragments and scroll step are passed directly to the native probe. It validates the probe envelope before consuming raw millisecond samples.

`scroll_p99_ms` uses nearest-rank P99 and `maximum_normal_stall_ms` is the maximum retained post-warmup frame sample.

### Warm exact search

`zevryon-m7-native-probe warm-search` uses boundary `open-once-one-warmup-v1`:

1. open one `StoreReader`;
2. perform one unmeasured exact-search warmup that must find the canonical query;
3. execute the canonical trial count on the same reader;
4. require every measured trial to find the query;
5. write raw millisecond samples.

The adapter reports nearest-rank P95 as `exact_search_warm_ms` and validates trial count and UTF-8 query byte count.

## Device-profile authority

The device profile is selected only from `system_state.physical_ram_mib`. The adapter intentionally ignores `ZEVRYON_DEVICE_PROFILE`; an environment override cannot relabel campaign evidence.

## Remaining readiness debt

Five canonical metrics remain unavailable and therefore keep the Zevryon raw-run failed:

- `process_group_pss_mb`
- `first_viewport_streaming_ms`
- `exact_search_cold_ms`
- `mutation_p95_us`
- `copy_throughput_mib_s`

The failed raw-run carries an empty metric object plus a bounded diagnostic listing the missing metrics and the four values that were actually measured. Partial measurements are never promoted to a successful nine-metric run.

## Semantic blockers

- Process-group PSS must measure the real benchmark process tree rather than reuse a device-profile target.
- Streaming first viewport must begin at raw corpus progressive import and end when the preview path actually produces a usable viewport; a prebuilt store is not streaming evidence.
- Cold search requires a fresh process for every trial and later campaign-level file-cache-state control. Reopening `StoreReader` in a warm process is insufficient.
- Current arena mutation APIs publish persistent metadata, so they must not operate on the canonical benchmark store. An isolated scratch fixture or reversible user-visible edit path is required.
- Full-document copy needs one cross-engine semantic boundary. Store traversal or disk export will not be renamed as copy merely because they move bytes.

## Tests

`m7-zevryon-readiness-adapter-smoke` injects fake frame and native-operation probes and verifies:

- canonical RAM-to-profile selection;
- 1000 retained scroll samples;
- nearest-rank scroll P99 and maximum stall;
- preindexed timing-envelope validation;
- nearest-rank warm-search P95;
- exactly five metrics remain unavailable.
