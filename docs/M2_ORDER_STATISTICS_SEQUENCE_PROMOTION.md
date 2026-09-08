# M2 — Order-Statistics Sequence Promotion

This receipt promotes the M2 durable logical-order authority only after the implementation head and the evidence-only publication head are independently exact-head green.

## Frozen source authority

- repository: `Naveax/Zevryon`
- branch: `agent/m2-order-statistics-sequence`
- base `main`: `b93c93c1df178d4335f8000eced890b8f36356d2`
- source authority head: `d17628a3d51922d50fd295f84b436e13e935846a`
- source relation to main: 21 ahead / 0 behind
- changed implementation/documentation/test paths: 33
- exact source CI: `Windows and Linux CI` run `31795891050`, SUCCESS

The source authority is therefore the exact runtime/test/documentation state being certified. This promotion commit is intentionally evidence-only and must not alter source, test, build, workflow or runtime behavior.

## Admitted M2 authority

The admitted line provides:

1. a bounded-chunk persistent order-statistics sequence with subtree aggregates;
2. O(1) immutable snapshots and O(1) shared-root transaction forks;
3. immutable physical `source_record_index` identity independent from mutable logical `record_index`;
4. logical move/reorder with final-index semantics;
5. payload, checkpoint, raw-window and persistent-height dereference through physical source identity after reorder;
6. generation-based durable logical-order persistence;
7. torn temporary publication recovery and fail-closed committed-generation validation;
8. Windows and Linux durable generation publication;
9. reopen recovery through `CompactArenaReader`, ordinary `LayoutWindowEngine`, and `ZenithHotScrollSession`.

Durable compact-arena insert/erase are explicitly not promoted by this receipt.

## Durable publication contract

A logical move does not publish the new live root first. It:

1. forks the current immutable root;
2. mutates only the candidate;
3. serializes the candidate physical-source permutation;
4. durably publishes a unique generation `arena/logical-order.g<16-hex-generation>.zmd`;
5. reloads and validates the committed generation;
6. only then publishes the candidate as the live root.

Older committed generations are never overwritten. Missing generations retain backward-compatible identity order. Torn `.tmp` candidates are ignored. A corrupt committed generation fails open closed rather than silently falling back to an older order.

## Physical-source invariant

After logical reordering:

- `record_index` is the current logical ordinal;
- `source_record_index` remains the immutable physical storage ordinal;
- source payload reads use `source_record_index`;
- checkpoint paths and checkpoint cache identity use `source_record_index`;
- hot-scroll raw source windows use `source_record_index`;
- `record-heights.idx` and `height-blocks.idx` remain physical-source keyed;
- fragments, anchors and logical sequence selection continue to use logical order.

The moved record therefore changes position without changing which physical payload/checkpoint/height state it owns.

## Evidence-head admission rule

The evidence-only promotion child adds only:

- `certification/m2_order_statistics_sequence_promotion.json`
- `docs/M2_ORDER_STATISTICS_SEQUENCE_PROMOTION.md`

Because these files are evidence-only, the source authority remains `d17628a3d51922d50fd295f84b436e13e935846a`. The evidence-only child had to receive its own successful push-triggered `Windows and Linux CI` run before promotion.

## Canonical promotion result

That admission rule was satisfied.

- evidence-only child head: `959e3b00c7b64b7fa96524905b9f81fe06a70d75`;
- required push-triggered exact-head CI: run `31796822424`, SUCCESS;
- PR exact-head CI: run `31797517189`, SUCCESS;
- promotion PR: #97, `M2: promote durable order-statistics sequence authority`;
- canonical merge commit: `e132538834b55bb2b40157997b20693201bf6f78`.

The historical JSON receipt was authored before the evidence-head run and therefore retains the pre-admission string `source-authority-green-awaiting-evidence-head-exact-ci`. That string is a snapshot of the state at receipt creation, not the current canonical status. The immutable run/PR/merge evidence above supersedes it for admission status without rewriting the historical receipt.

A later fresh-main residual physical-source audit found no production logical-ordinal-to-physical-source dereference regression. Durable arbitrary compact-arena insert/erase remain explicit non-capabilities and are not silently added by this reconciliation.
