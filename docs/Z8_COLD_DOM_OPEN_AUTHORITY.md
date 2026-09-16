# Z8 cold DOM open authority

## Scope

This authority certifies four properties of the existing logical-node storage and hot semantic projection surfaces without promoting Z8 as a whole.

The focused cross-platform workflow exercises:

- stable logical node identity and authoritative source binding through the store-bound v2 arena;
- lazy bounded semantic-node projection through `ZenithSemanticNodeWindow` and `ZenithSemanticRuntimeConsumer`;
- a one-million-node cold logical arena whose on-disk sidecar footprint stays at or below 97 logical bytes per node;
- one-million-node open plus direct last-node seek without materializing or scanning the complete node table.

The million-node fixture uses the production v1 `LogicalNodeArenaReader` format with a sparse 96-byte fixed-record node table, valid manifest and dictionary records, and valid CRCs for the root and last node that are actually read. This isolates the cold-open property from arena construction cost while still executing the production reader and validation paths.

## Frozen gates

- node count: exactly 1,000,000;
- logical sidecar bytes: at most 97,000,000 bytes;
- cold open latency: at most 2,000 ms;
- direct lookup of logical node 1,000,000: at most 2,000 ms;
- final logical ID and parent ordinal must decode exactly;
- semantic dictionary resolution for the root tag must remain exact;
- Linux and Windows focused authorities must both pass.

The latency ceilings are intentionally loose guardrails against an accidental whole-arena scan. They are not browser-frame latency claims.

## Explicit boundary

This slice does not claim `dom_traversal_conformance`. It does not expose Web IDL DOM wrappers, mutation semantics, live collections, selector traversal, event targets, JavaScript wrappers, or a complete browser DOM API. Z8 remains unpromoted until that remaining gate is implemented and independently certified.
