# Z3 bounded selector dependency invalidation foundation

This slice adds the first production dependency-key and semantic-change
invalidation primitive for Z3. It does not promote Z3 and does not admit the
full `selector_dependency_invalidation` gate.

## Dependency keys

`build_css_selector_dependency_set_v1` derives a bounded dependency set from
one already-compiled compound selector:

- type selector → tag dependency;
- ID selector → `id` attribute dependency;
- class selector → `class` attribute dependency;
- attribute existence/equality → named attribute dependency;
- universal selector → no semantic dependency.

Repeated class/ID/tag dependencies collapse to one key. Named attribute
dependencies deduplicate with HTML ASCII-case-insensitive name comparison.

Named attribute keys are copied into dependency-set PMR text storage in
canonical ASCII-lowercase form. The dependency set therefore does not borrow
selector text and remains valid if the source selector object is later replaced
or released. This avoids cross-selector aliasing entirely instead of relying on
a probabilistic fingerprint.

## Semantic changes

`css_selector_dependencies_invalidated_v1` consumes a small semantic mutation
summary: `tag_changed` plus changed attribute names. A matching dependency
returns a conservative may-affect-match result. Attribute renames must report
both the old and new names.

The foundation intentionally invalidates by dependency name, not by attempting
to prove that the selector's final truth value changed. This trades harmless
extra invalidation for the more important property that supported dependency
changes do not become false negatives.

## Bounds

Dependency count, changed-attribute count, changed-name semantic bytes and
comparison work units all have configurable ceilings with fixed implementation
maxima. Dependency storage uses the caller PMR resource; focused tests charge
it to `ResourceClass::ComputedStyle` and exercise real hard-limit rejection.

Corrupt selector records, corrupt dependency records and malformed mutation
summaries fail closed. Extracted dependency sets own their named keys, so they
remain valid after the source selector is replaced or released. Dependency-set
replacement is atomic on extraction failure.

## Boundary

There is no store-bound mutation feed yet. Combinator ancestry/sibling
propagation, pseudo-class state, selector lists, stylesheet replacement,
subtree invalidation, cascade recomputation, computed-style DAG invalidation
and offscreen materialization remain separate work. The existing bounded
`ZenithSemanticNodeWindow` is the intended store-facing semantic source for a
later integration slice.
