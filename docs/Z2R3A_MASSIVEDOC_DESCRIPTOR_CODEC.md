# Z2R-3A — MassiveDoc descriptor codec foundation

## Objective

Begin the next Rust-first core migration after `ResourceLedger` authority promotion without changing the production MassiveDoc writer or reader.

This slice moves the fixed on-disk descriptor contract and overflow-sensitive range arithmetic into safe Rust, then certifies exact compatibility from Rust, C11 and C++20.

## Existing disk contract

The production C++ store currently uses:

- a 32-byte little-endian record descriptor;
- a 24-byte little-endian chunk descriptor;
- four reserved chunk bytes at offsets 4 through 7;
- record-table offsets calculated as `record_index * 32`;
- chunk-table offsets calculated as `chunk_index * 24`.

The Rust codec reproduces this layout exactly. Chunk decoding intentionally ignores the reserved four bytes because the existing C++ parser does the same; Rust encoding always writes them as zero.

## Rust boundary

`zevryon-massivedoc` is safe Rust and forbids unsafe code. It owns:

- record descriptor encoding and decoding;
- chunk descriptor encoding and decoding;
- checked descriptor-table offset arithmetic;
- bounded record-slice planning;
- checked chunk-within-segment validation;
- checked record chunk-span validation.

`zevryon-massivedoc-ffi` is a separate fixed C ABI static library. Unsafe code is limited to validated foreign pointer reads, writes and byte-slice construction.

No allocator, Rust collection, C++ STL object, exception or panic crosses the boundary.

## Fail-closed behavior

The ABI rejects:

- null descriptor or output pointers;
- misaligned structured outputs;
- descriptor buffers with a non-exact size;
- record and chunk table offset overflow;
- record slice offsets beyond the record length;
- chunk ranges beyond the configured segment boundary;
- chunk spans that overflow or exceed the chunk table.

## Certification

The focused matrix runs on Ubuntu, Windows and macOS:

- `cargo fmt --check`;
- all Rust workspace tests;
- Clippy with `-D warnings`;
- release static-library build;
- strict C11 build and runtime ABI test;
- strict C++20 build and exact disk-layout equivalence test.

The C++ oracle covers zero values, ordinary values and maximum-width integer values. It also proves reserved chunk bytes remain decode-compatible while Rust encoding canonicalizes them to zero.

## Authority boundary

This slice does not make Rust authoritative for MassiveDoc storage. The production writer, reader, file I/O, CRC32, SHA-256, search signatures and manifests remain C++.

The next slice may integrate the Rust codec as an optional production shadow around descriptor serialization/parsing. Promotion requires exact corpus, payload hash, exported payload and giant-record parity with an immediate build-time rollback path.
