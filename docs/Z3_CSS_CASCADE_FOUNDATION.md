# Z3 bounded author-rule cascade foundation

This slice adds the first production cascade winner primitive for Z3. It does
not promote Z3 and does not admit the full `cascade_conformance` gate.

## Production surface

`cascade_css_author_rules_v1` evaluates one already-parsed author stylesheet
against one semantic node. Selectors are compiled and matched through the
bounded compound-selector foundation. For each matched declaration the winner
order is:

1. `!important` over normal declarations;
2. higher selector specificity;
3. later declaration source order.

Winner records borrow property/value slices from `CssStylesheetV1::text`.
Retained winner storage and temporary compiled selectors use the caller's PMR
resource. Focused tests charge that storage to
`ResourceClass::ComputedStyle`.

## Frozen upstream authority

The focused scope pins
`web-platform-tests/wpt@15df54d4459b78242d32ae36f9c094a96972bedc`
and exact blobs for three cases that fit the currently supported selector
surface:

- `specificity-001.xht`: attribute selector beats a later type selector;
- `specificity-007.xht`: type selector beats a later universal selector;
- `cascade-005.xht`: later source order wins when weight and specificity tie.

The oracle also covers author-origin `!important` precedence directly. It
does not reinterpret upstream tests that require combinators, pseudo-classes,
user stylesheets, inline-style precedence, inheritance or shorthand expansion.

## Bounds and failure model

Rule count, declaration count, unique emitted properties and cascade work units
all have configurable hard ceilings with fixed implementation maxima. Property
lookup consumes work units proportional to compared property bytes. Selector
matching also consumes the same aggregate cascade budget using the bounded node
semantic size, attribute count and compiled simple-selector shape, so thousands
of individually valid matches cannot bypass the global work ceiling. Unsupported
selector semantics, corrupt parser slices, match-budget rejection and resource
allocation failure all fail closed.

Output replacement is atomic: a failed evaluation leaves the caller's previous
winner set untouched.

## Boundary

This is not full CSS Cascade conformance. User/user-agent origins, layers,
inline-style origin precedence, inheritance, initial/unset/revert semantics,
shorthand expansion, computed values, logical-DOM integration, invalidation and
computed-style DAG sharing remain separate Z3 slices.
