# Z1 core seek regression gate

Z1's Unicode substrate must not regress the bounded MassiveDoc seek path that feeds text processing. This gate reuses the production ZENITH hot-scroll session and the canonical 128 MiB corpus containing one real 64 MiB record.

## Immutable limits

The limits are inherited from `config/zenith_program.json` and are not benchmark-tuning suggestions:

- random hot-scroll P95: at most `0.50 ms`;
- random hot-scroll P99: at most `0.75 ms`;
- adjacent hot-scroll P95: at most `0.30 ms`;
- physical source read per query: at most `65,536 bytes`;
- sparse checkpoint overhead: at most `0.002` of source bytes;
- silent payload corruption: zero;
- payload data loss: zero bytes.

The exact-head authority also preserves the existing adjacent zero-I/O ratio of at least 95%, zero warmed checkpoint reparses, and both byte-budgeted caches below their configured hard limits.

## Workload

The workflow creates the canonical deterministic 128 MiB MassiveDoc benchmark corpus with 131,072 records and a 64 MiB giant record at record index 65,536. It then performs the production checkpoint-aware baseline and 257-query random and adjacent in-process hot-scroll profiles with a 16 KiB checkpoint stride.

The gate measures storage/indexing and bounded source access. It does not treat hosted-runner timing as an end-user browser promise; its purpose is regression rejection against the existing core contract.
