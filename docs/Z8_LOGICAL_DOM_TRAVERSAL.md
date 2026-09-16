# Z8 logical DOM traversal authority

## Scope

`LogicalDomTraversal` exposes bounded read-only tree navigation over the authoritative `LogicalNodeArenaV2StoreBoundReader`. It does not create a parallel DOM tree and does not copy the arena into memory.

The production surface supports:

- node lookup by ordinal;
- parent and first-child navigation;
- next- and previous-sibling navigation;
- forward and reverse preorder traversal;
- strict-descendant checks;
- document-order comparison.

Logical-node ordinals are the arena's stable pre-order identities, so document order is represented directly by ordinal order. Every record read still passes the existing store-bound v2 identity, CRC, range and topology validation.

## Bounded traversal

Relations that may walk ancestor or sibling chains use an explicit hop budget. The default maximum is 1,048,576 hops and configuration is rejected above 16,777,216 hops. A malformed or unexpectedly long chain fails closed instead of allowing unbounded traversal work.

Sibling navigation additionally verifies that every traversed sibling has the expected parent. Preorder navigation verifies child and sibling topology while climbing or descending the tree.

## Certification

The focused authority builds a real native store and a real store-bound v2 logical-node arena containing a nine-node mixed-depth tree. Linux and Windows execute the production traversal surface and require exact results for all certified relations, out-of-range failure, bounded-hop rejection and invalid-configuration rejection.

The frozen scope file is `certification/z8_dom_traversal_scope.json` and is independently checked by `tests/test_z8_dom_traversal_scope.py`.

## Boundary

This gate is internal logical-tree traversal conformance. It does not claim Web IDL DOM bindings, JavaScript wrappers, live `NodeList`/`HTMLCollection` behavior, mutation semantics, selector matching, events or shadow DOM. Those belong to later runtime/web-platform milestones rather than this Z8 storage/projection gate.
