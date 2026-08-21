# M7 canonical competitor workload

## Purpose

The M7 adapter protocol identifies a workload by SHA-256. This contract defines the exact schema that concrete browser adapters must execute so identical hashes also mean identical benchmark semantics.

## Workload schema

Schema identifier: `zevryon.m7.workload.v1`.

The top-level object contains the exact corpus SHA-256 and logical byte count, one viewport object and the ordered canonical operation set.

Viewport fields:

- `width_px`
- `height_px`
- `overscan_px`
- `max_fragments`

Operations must appear exactly in this order:

1. `open_preindexed`
2. `open_streaming`
3. `scroll`
4. `exact_search_warm`
5. `exact_search_cold`
6. `mutation_batch`
7. `copy_all`

The scroll operation carries measured sample count, warmup count and step distance. At least 1000 post-warmup samples are required and the total retained observation count may not exceed the native frame-probe cap.

Warm and cold exact-search operations use the same UTF-8 query. Cold search requires `fresh_process_each_trial=true`; merely constructing a new Zevryon `StoreReader` while operating-system file pages remain warm is not admitted as a cold-search run.

Mutation and full-copy trial counts are bounded and explicit. Array order is significant in the workload SHA-256 because benchmark operation order is part of the experiment.

## Fail-closed normalization

`parse_canonical_workload()` accepts only the exact field set. Unknown viewport fields, missing operations, reordering, non-canonical cold-search semantics, different warm/cold queries and out-of-range counts are rejected.

The parser round-trips the parsed object back to the canonical JSON structure and requires exact equality. Adapters therefore do not silently normalize or reinterpret benchmark requests.

## Current Zevryon readiness

The existing `zevryon-zenith-frame-probe` already satisfies the canonical scroll timing primitive: it measures full `ZenithTabRuntime::layout()` calls, discards an explicit warmup set and emits raw millisecond samples. The M7 Zevryon adapter reuses that primitive rather than introducing a second frame-timing definition.

Other metrics are admitted only after an equivalent real primitive exists. In particular, a fresh `StoreReader` is not sufficient evidence for OS-cold search, and the current persistent arena mutation API must not be benchmarked against the canonical source store until an isolated mutation fixture is used.
