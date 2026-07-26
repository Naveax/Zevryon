# Z2F-3: Bounded Device-Pixel Raster Execution and Upload Completion

## Scope

Z2F-3 consumes the Z2F-2 viewport glyph working set and a sorted snapshot of
currently resident raster keys. It emits work only for cold keys, executes those
jobs through a backend-neutral two-pass raster ABI, publishes immutable
`GlyphRasterSourceRecord` payloads, and validates generation-safe atlas upload
completion.

The cost is bounded by cold viewport glyph keys and upload batches. Total
document glyph count does not affect the stage.

## Residency boundary

The planner accepts a sorted unique `resident_keys` snapshot. Keys present in
that snapshot are treated as hot and produce no raster job. Z2F-2 remains the
authoritative atlas cache during submission; a stale or incomplete residency
snapshot cannot corrupt the cache because all raster sources retain exact key
identity and Z2F-2 validates every publication.

This explicit snapshot avoids exposing mutable atlas internals to raster worker
threads.

## Device raster policy

`DeviceRasterPolicy` binds one immutable policy ID to:

- 16.16 device-scale multipliers;
- grayscale and LCD subpixel phase grids;
- maximum raster dimension and bytes per glyph;
- hinting policy;
- LCD filter policy;
- color-glyph acceptance policy.

The policy ID is carried in the existing compact raster key's reserved byte.
Keys from a different policy fail closed instead of aliasing resident atlas
content.

## Job planning

`build_device_glyph_raster_plan`:

- validates sorted unique resident keys;
- rejects invalid font generations, faces, scales and subpixel phases;
- requires one immutable verified face byte-span for every cold key;
- applies checked 16.16 device scaling;
- emits exactly one job per cold raster key;
- records queue and atlas generation snapshots;
- enforces an explicit maximum job count;
- publishes output atomically through PMR.

## Raster backend ABI

`DeviceGlyphRasterBackend` is a two-pass interface:

1. `query` returns exact dimensions, row bytes, bearings, format and payload
   size without writing pixels.
2. `render` receives an exact destination span and may not resize or retain it.

This ABI is suitable for FreeType, CoreText and DirectWrite adapters while
keeping memory ownership in Zevryon.

`ReferenceDeviceGlyphRasterBackend` is a deterministic CPU certification
backend. It supports:

- Alpha8 grayscale coverage;
- RGB LCD coverage;
- BGRA color glyph output;
- explicit empty-glyph results;
- deterministic metrics and byte output from raster identity.

It is not a typographic production rasterizer. Its role is to certify queue,
policy, memory, cache and upload invariants independently of platform APIs.

## Worker result publication

`execute_device_glyph_raster_plan`:

- rejects stale queue generations;
- revalidates face generation, face ID and resource ID;
- queries every job before allocating payload memory;
- validates exact `row_bytes * height` accounting;
- enforces per-glyph and aggregate payload limits;
- allocates source records and bytes exactly once;
- renders into non-overlapping exact spans;
- computes FNV-1a payload checksums expected by Z2F-2;
- publishes `GlyphRasterSourceRecord` objects directly;
- leaves output empty after every failure.

## Upload execution and fences

`execute_glyph_atlas_uploads` consumes a current Z2F-2 submission and the raster
payload referenced by its upload records. Consecutive uploads are grouped into
maximal batches with equal:

- atlas generation;
- page generation;
- page index;
- raster format.

The backend receives immutable upload records and the original payload span.
Each successful batch returns a strictly increasing fence value.

`GlyphAtlasUploadReceipt` retains the ticket, fence, generation and upload
range. `glyph_atlas_upload_execution_is_current` rejects:

- cache clears;
- page-generation changes;
- stale submissions;
- incomplete receipts;
- non-monotone fences;
- receipt/batch topology mismatches.

## Compact records

| Record | Bytes |
|---|---:|
| `DeviceRasterPolicy` | 32 |
| `DeviceGlyphRasterJob` | 80 |
| `DeviceGlyphRasterMetrics` | 32 |
| `GlyphAtlasBackendUploadBatch` | 32 |
| `GlyphAtlasUploadReceipt` | 48 |

Z2F-3 reuses the Z2F-2 88-byte raster source and 64-byte upload records.

## Failure model

The stage fails closed for:

- malformed or policy-mismatched raster keys;
- unsorted or duplicate residency snapshots;
- missing or generation-mismatched face sources;
- checked device-scale overflow;
- stale queue generations;
- backend query or render failure;
- malformed raster metrics;
- source, payload, job or upload-batch limits;
- stale atlas submissions during upload;
- non-monotone backend fences;
- PMR allocation failure;
- aggregate arithmetic overflow.

Plan, source and upload outputs remain empty after failure. Z2F-2 cache
publication remains separately transactional.

## Certification fixture

The fixed fixture represents:

- a 16,384-line source document;
- an 80-line viewport projection;
- 96 unique viewport raster keys;
- 320 glyph uses;
- 64 grayscale, 16 LCD and 16 color keys;
- 96 cold jobs and zero hot jobs with a complete residency snapshot;
- deterministic empty-glyph handling;
- cold Z2F-2 atlas publication and upload execution;
- hot Z2F-2 reuse with zero uploads;
- deterministic checksum across three hosted distributions.

The independent oracle covers 8,064 combinations of resource identity, raster
mode, scale, subpixel grid and glyph ID.

## Explicit boundary

Z2F-3 does not yet ship FreeType, CoreText or DirectWrite adapters; parse
COLR/CPAL or SVG paint graphs; select bitmap strikes from SFNT tables; define
platform hinting equivalence; create GPU textures; issue Vulkan/Metal/D3D
commands; wait on real GPU fences; composite surfaces; or present frames.
Those remain backend adapter and compositor stages after this contract.
