# Z3 bounded CSS selector foundation

This slice introduces the first production selector compiler/matcher primitive
for Z3. It deliberately does not promote Z3 and does not admit either
`cascade_conformance` or `selector_dependency_invalidation`.

## Production surface

`compile_css_compound_selector_v1` compiles one bounded compound selector into
flat PMR-backed records. `match_css_compound_selector_v1` evaluates those
records against a semantic node view. Retained selector storage is allocated
through the caller-provided memory resource; Z3 callers use
`ResourceClass::ComputedStyle`.

The admitted foundation syntax is:

- universal and type selectors;
- ID selectors;
- class selectors;
- attribute existence;
- exact attribute equality.

Specificity is represented as three bounded 32-bit components: ID,
class/attribute and type. Comparison is lexicographic in that order. Universal
selectors contribute zero.

## Matching semantics

For the HTML-oriented foundation, type and attribute names compare
ASCII-case-insensitively. ID and attribute values remain case-sensitive.
Classes are matched as ASCII-whitespace-separated tokens.

Matching is fail-closed and separately bounded. The configured node attribute
count, total semantic bytes (tag plus attribute names/values), and conservative
match work-unit estimate must all remain inside their limits before selector
evaluation begins. This prevents a small compiled selector API from becoming
an accidental unbounded scan over attacker-sized semantic input.

## Boundary

No combinators, selector lists, pseudo-classes, pseudo-elements, namespaces,
advanced attribute operators/flags or CSS identifier escapes are claimed.
There is not yet a store-bound logical-DOM adapter, cascade winner selection or
dependency invalidation. Those remain separate Z3 authorities.
