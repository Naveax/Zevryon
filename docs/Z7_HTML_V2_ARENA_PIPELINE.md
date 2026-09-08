# Z7 HTML v2 parser-to-arena production pipeline

## Purpose

The repository already contains independently admitted components for:

- streaming HTML parsing into `ZVNSRC01` v2;
- importing `ZVNSRC01` v2 into the physical-store-bound `node-arena-v2/` representation;
- bounded runtime semantic reads through `ZenithSemanticRuntimeConsumer`.

Those components previously had no single production orchestration boundary. `build_streaming_html_node_arena_v2()` joins them without weakening their individual validation or publication contracts.

## Pipeline

The combined path is:

1. read the authoritative native store through the bounded streaming HTML v2 parser;
2. publish a caller-selected create-only `ZVNSRC01` v2 source artifact;
3. import that exact source through `import_logical_node_source_v2_to_arena_v2()`;
4. publish the store-bound `node-arena-v2/` tree;
5. leave both source and arena available for independent replay/validation and bounded runtime consumption.

The pipeline does not bypass source/store binding checks. The importer still reopens and validates the source binding, validates every decoded node source span against the authoritative native store and relies on the store-bound arena writer to independently re-inspect physical source identity.

## Artifact path isolation

The caller-selected source artifact is orchestration evidence, not part of the authoritative native store. Its parent directory must therefore resolve outside the `store_root` tree.

The pipeline resolves the physical store path and source parent path before parser execution and rejects a source target whose parent is the store itself or one of its descendants. This blocks the combined pipeline from creating its own sidecar inside the source tree it is supposed to bind. The rejection happens before parser execution, source publication or arena staging.

The source parent directory must already exist. This matches the create-only writer boundary and avoids silently creating caller-controlled directory structure as part of parser execution.

## Transaction boundary

Parser publication arms rollback only after `produce_streaming_html_node_source_v2()` returns success. Therefore a pre-existing caller file that causes create-only parser publication to fail is never deleted by the pipeline.

If the parser succeeds but import later fails:

- the source artifact published by that invocation is removed;
- defensive source `.building` cleanup is attempted;
- importer-owned arena staging remains subject to the importer's existing fail-closed cleanup contract;
- the original import failure remains the primary error, with any rollback failure appended explicitly.

If an authoritative `node-arena-v2/` already exists, the existing create-only arena writer rejects replacement. The new source produced by the failed retry is rolled back while the existing arena and any earlier evidence source remain untouched and runtime-readable.

If import succeeds, both source and arena remain published. Keeping the source is intentional: it preserves the exact parser-to-arena evidence boundary rather than making arena output impossible to replay independently.

## Runtime authority

Focused end-to-end authority constructs a real native HTML store and runs:

`native store -> HTML v2 parser -> ZVNSRC01 v2 -> arena-v2 -> ZenithSemanticRuntimeConsumer`

The runtime read verifies that document, element and text semantics, parent topology and raw RCDATA source identity survive the complete path.

Additional tests cover:

- invalid import configuration after parser success rolls the owned source back;
- parser failure publishes neither source nor arena;
- a pre-existing source artifact is preserved byte-for-byte when create-only parser publication fails;
- an existing authoritative arena rejects replacement, rolls back the retry source and remains runtime-readable;
- a source artifact inside the authoritative native store is rejected before parser execution;
- no source or arena staging tree remains after admitted failure paths.

## Admission boundary

This slice closes an orchestration gap, not the full Z7 milestone. The HTML v2 parser remains a strict incremental profile. Script-data, PLAINTEXT, complete input preprocessing, decoded RCDATA text payload semantics, full WHATWG tree building/recovery, foreign content, parser fuzzing and the configured Z7 conformance/large-document gates remain outstanding.

Hosted CI for this path is implementation authority only and is not M7/M8 physical certification evidence.
