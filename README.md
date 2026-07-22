# Zevryon Browser — MassiveDoc Mainline

Zevryon is an independent browser-engine research project centered on a disk-backed **Logical Document** and a bounded **Render Working Set**.

The current mainline has one primary contract:

> Preserve and operate on a 4 GiB adversarial logical document without resident full-source/full-DOM state, data loss, or unbounded process memory.

Release numbering is paused. Development uses evidence-backed milestones on `main`.

## Memory contract

Memory is measured in **decimal MB**, not MiB.

| Profile | Target process-group PSS | Absolute survival cap |
|---|---:|---:|
| Legacy phone | **64 MB** | **80 MB** |
| Mid phone | **80 MB** | **96 MB** |
| Modern phone | **96 MB** | **128 MB** |
| Desktop | **128 MB** | **160 MB** |

Additional RAM is optional cache capacity, not permission to exceed the contract. Pressure control reduces caches, overscan, image decoding, background work, and materialized accessibility state. Logical source bytes are never discarded.

## Certification envelope

| Dimension | Required load |
|---|---:|
| Logical UTF-8 payload | **4 GiB** |
| Logical records | **8,388,608** |
| Logical nodes | **67,108,864** |
| Style runs | **33,554,432** |
| Resource references | **1,048,576** |
| Largest record | **64 MiB** |
| Largest unbroken token | **16 MiB** |

## Implemented M1 foundation

The native `zevryon-massivedoc` executable now provides:

- segmented disk-backed payload storage;
- records spanning multiple segment files;
- fixed-width record and chunk indexes;
- per-record CRC32 and full-payload SHA-256;
- false-negative-free block bigram search signatures;
- bounded record-slice materialization;
- streaming verification and export;
- corruption detection;
- browser document-process commands: `MASSIVE_OPEN`, `MASSIVE_STATS`, `MASSIVE_FIND`, and `MASSIVE_RECORD`.

The native store does **not** materialize the complete payload or descriptor set in RAM.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
PYTHONPATH=. python -m pytest -q tests
```

## MassiveDoc commands

```bash
python scripts/generate_massivedoc_corpus.py corpus.zmdoc \
  --logical-bytes 67108864 --records 131072

build/zevryon-massivedoc import corpus.zmdoc store 16
build/zevryon-massivedoc stats store
build/zevryon-massivedoc search store ZEVRYON_WORST_CASE_TAIL_MARKER 2
build/zevryon-massivedoc verify store
build/zevryon-massivedoc export store payload.bin
```

Run the reproducible smoke benchmark:

```bash
PYTHONPATH=. python scripts/massivedoc_benchmark.py \
  --logical-bytes 67108864 --records 131072 \
  --output evidence/milestones/m1-native-store-64mib.json
```

## Current limitation

The disk-backed store and document-process protocol are implemented. Full HTML/CSS layout, JavaScript, accessibility, and paint are not yet reading directly from compact MassiveDoc nodes; the next milestone is the compact node arena plus viewport materializer. Zevryon is not a safe daily browser for sensitive accounts.
