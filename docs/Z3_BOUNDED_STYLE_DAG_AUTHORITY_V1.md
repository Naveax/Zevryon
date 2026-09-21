# Z3 bounded style-DAG authority v1

This authority admits the Z3 `bounded_style_dag` gate without promoting Z3.

The production surface is `intern_css_cascade_style_v1`. The authority keeps
the existing focused foundation oracle and adds a deterministic stress corpus
that exercises sharing identity and hard bounds on the actual production DAG.

## Exact denominator

The stress authority interns 512 unique final styles. Every style has the same
canonical `a:1; b:2` prefix and one unique `zN:vN` suffix. Source declaration
order is intentionally scrambled before the cascade result reaches the DAG.

The authority then replays all 512 final styles through a different cascade
history: the unique property is won through a class selector with
`!important`, while the final property/value set remains identical.

Passing requires exactly:

- 512 unique fanout styles;
- 512 provenance replays;
- 515 retained nodes: root + shared `a` + shared `b` + 512 unique suffixes;
- 3880 retained text bytes;
- zero new nodes and zero appended text on every provenance replay.

This makes the sharing claim countable instead of hand-waving at a couple of
green examples, a pastime software projects have somehow industrialized.

## Boundary authority

The same production surface must also pass:

- node-limit rollback after a partial extension opportunity;
- text-limit rollback after a partial extension opportunity;
- property-count rejection;
- per-style semantic-byte rejection;
- work-unit rejection;
- retained topology corruption rejection;
- real `ResourceClass::ComputedStyle` hard-limit allocation rejection with
  clean accounting.

The existing foundation oracle remains required and covers canonical property
ordering, empty-root identity, prefix sharing, corrupt ordering and the basic
rollback surfaces.

## Claims and nonclaims

This scope admits only `bounded_style_dag`. It does not claim inheritance,
initial/unset/revert semantics, shorthand expansion, computed-value
normalization, invalidation propagation, offscreen materialization or full CSS
computed-style semantics.

Both Ubuntu 24.04 and Windows 2022 must execute the same production authority.
