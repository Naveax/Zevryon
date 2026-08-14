# M3 — Storage Hardening Promotion

This receipt promotes the M3 storage-hardening authority only after the implementation source head and the evidence-only publication head are independently exact-head green.

## Frozen source authority

- repository: `Naveax/Zevryon`
- branch: `agent/m3-crash-safe-generations`
- base `main`: `e132538834b55bb2b40157997b20693201bf6f78`
- source authority head: `d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0`
- source relation to main: 15 ahead / 0 behind
- changed implementation/test/build/workflow paths: 44
- exact source CI: `Windows and Linux CI` run `31814283673`, SUCCESS

The source authority above is the exact runtime/test/build/workflow state being certified. This promotion commit is intentionally evidence-only and must not alter implementation, tests, build wiring, workflow behavior, or runtime behavior.

## Admitted M3 authority

The admitted line provides:

1. immutable segmented source generations with explicit source identity and segment inventory;
2. checksummed append journal with PREPARE/COMMIT publication states;
3. crash recovery that admits only fully committed generations and quarantines corrupt/torn metadata;
4. bounded positional `pread`/`ReadFile` I/O, including offsets beyond 4 GiB;
5. generation-local compaction with explicit publication ordering and recovery boundaries;
6. single-flight bounded background compaction with request coalescing, pending cancellation, and serialized publication transactions;
7. bounded hot/warm/cold immutable-block admission, promotion, demotion, eviction, and ResourceLedger accounting;
8. StoreReader integration of the shared hot/warm cache across record, slice, and search payload reads;
9. an exact 4 GiB canonical-generation StoreReader open under the legacy profile with bounded head/tail access and measured PSS below the 64 MB target and 80 MB hard cap;
10. immutable progressive prefix publication so a verified StoreReader + compact-arena first viewport is usable while primary import/index work is still incomplete;
11. a bounded read-only file-backed cold mapping window whose resident segment pages are directly included in Linux process PSS while hot/warm resident bytes are zero.

## Exit-gate evidence

All M3 execution-plan exit gates are admitted on the frozen source authority.

### Exact 4 GiB legacy-profile open

- exact logical payload: `4,294,967,296` bytes;
- canonical-generation StoreReader open: PASS;
- bounded head and tail payload reads: PASS;
- sparse fixture allocation remains bounded rather than materializing 4 GiB of zeros;
- legacy 64 MB PSS target: PASS;
- legacy 80 MB hard cap: PASS;
- no crash/OOM: PASS.

### First viewport before primary completion

The exact cross-platform CTest publishes an immutable preview after record 1 of a 3-record import while 2 primary records remain pending. At callback time:

- the primary store is intentionally still uncommitted/unopenable;
- the preview generation opens and verifies independently;
- the current partial search block is queryable;
- `CompactArenaReader` opens the preview arena;
- the first viewport materializes the prefix record;
- primary import subsequently completes without mutating preview authority.

The first preview record spans three small segments, so this is a multi-segment prefix-publication proof rather than a single-file shortcut.

### Resident cold-store pages count against PSS

Linux authority uses `/proc/<pid>/smaps` and `/proc/<pid>/smaps_rollup`, not ResourceLedger as a substitute for OS memory accounting.

Exact source-head cold evidence:

- baseline total PSS: `1.145856 MB`;
- 4 MiB cold-mapped total PSS: `5.344256 MB`;
- total PSS increase: `4,198,400` bytes;
- direct `segments/*.bin` file-backed PSS: `4,194,304` bytes;
- hot/warm resident bytes during the cold hold: `0`;
- cold mapped bytes / peak mapped bytes: `4,194,304 / 4,194,304`;
- cold touched bytes: `4,194,304`;
- SourceWindow hard-limit/accounting checks: PASS;
- legacy target/hard cap: PASS;
- no crash/OOM: PASS.

This proves resident MassiveDoc cold-store pages are visible in process PSS. It does not claim unmapped kernel page-cache bytes, anonymous heap bytes, or unbounded mmap residency as cold-store accounting.

## Exact source-head CI

`Windows and Linux CI` run `31814283673` completed SUCCESS on `d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0`:

- Linux build + headless `94812100317`: SUCCESS, 84/84 CTest, 4 GiB gate PASS, cold-store PSS gate PASS;
- Windows build + headless `94812100352`: SUCCESS, 48/48 CTest;
- Linux Unicode authority `94812100373`: SUCCESS;
- Windows Unicode authority `94812100383`: SUCCESS;
- Apple backend removal guard `94812100437`: SUCCESS.

Storage evidence artifact: `m3-storage-d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0`, artifact ID `9224429305`.

## Deliberate boundaries

This receipt does not claim:

- durable compact-arena arbitrary insert/erase beyond the already admitted M2 move/reorder boundary;
- unbounded whole-corpus memory mapping;
- unmapped kernel page-cache residency as process PSS;
- full 4 GiB payload SHA scanning as part of the open-only exit gate;
- that ordinary StoreReader cache misses automatically use mmap. The cold mapping is an explicit bounded residency window; ordinary payload reads remain bounded positional I/O.

## Evidence-head admission rule

This promotion commit adds only:

- `certification/m3_storage_hardening_promotion.json`
- `docs/M3_STORAGE_HARDENING_PROMOTION.md`

Because those files are evidence-only, the source authority remains `d8d9f11d1bcc1dea12b82d0fa9b2b3f69aa1d9c0`. The branch is not fully promoted merely because this receipt exists. The exact evidence-only child head must itself receive a successful push-triggered `Windows and Linux CI` run.

Only after that child head is 5/5 green is the M3 promotion chain closed and ready for PR/repository promotion handling. No merge credit is granted by this receipt alone.
