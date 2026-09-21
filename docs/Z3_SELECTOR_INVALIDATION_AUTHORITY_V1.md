# Z3 selector dependency invalidation authority v1

This authority admits the configured Z3
`selector_dependency_invalidation` gate for the production compound-selector
profile without promoting Z3 as a whole.

## Production path

Every authority selector runs through:

1. `compile_css_compound_selector_v1`;
2. `build_css_selector_dependency_set_v1`;
3. `css_selector_dependencies_invalidated_v1`.

The authority does not manufacture dependency records directly.

## Exact decision denominator

Five independent dependency dimensions are combined into every bit-mask from
0 through 31:

- tag identity;
- `id` attribute;
- `class` attribute;
- `data-a`;
- `data-b`.

Mask zero is represented by the universal selector. The other masks construct
one compound selector from exactly the enabled dimensions.

Each of the 32 profiles is evaluated against eight semantic mutation summaries:

- tag change;
- `id` change;
- `class` change;
- `data-a` change;
- mixed-case `DaTa-B` change;
- unrelated `title` change;
- `id` plus unrelated `title`;
- rename-style `data-b` plus `data-c`.

That freezes exactly **256 invalidation decisions**. Every decision must equal
the dependency-mask expectation. There is no unsupported or xfail bucket.

After dependency extraction, the source selector object is deliberately
recompiled to an unrelated selector before the mutation matrix runs. This
proves that named dependency keys are self-contained rather than borrowing the
compiled selector text.

## Additional bounds

A separate selector containing repeated class, ID and named-attribute
dependencies verifies exact deduplication.

A five-dependency selector is also evaluated with a four-dependency ceiling and
must fail with `DependencyLimitExceeded`. A tiny invalidation work budget must
fail with `WorkBudgetExceeded` and leave the published invalidation result
false.

## Platform authority

The dedicated workflow runs the foundation invalidation oracle and this exact
authority matrix on Ubuntu 24.04 and Windows Server 2022. Both platforms must
pass on the same exact head.

## Boundary

This gate is deliberately scoped to the production compound-selector profile:
type, universal, ID, class, attribute-existence and attribute-equality
dependencies.

It does not claim combinator ancestry/sibling propagation, pseudo-class state,
pseudo-elements, selector lists or broader selector grammar. Those semantics
must not be inferred from this gate name.
