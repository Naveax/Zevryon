# Z3 bounded computed-style DAG authority v1

This authority admits the configured Z3 `bounded_style_dag` gate without
promoting Z3 as a whole.

## Production path

The authority does not construct DAG records by hand. It runs the production
chain:

1. `parse_css_stylesheet_v1`;
2. `cascade_css_author_rules_v1`;
3. `intern_css_cascade_style_v1`.

The generated stylesheet contains 32 class-selected computed styles. Every
style has eight winning properties. Seven property/value pairs are identical
across all styles and the eighth differs per style. Declaration order alternates
between canonical and reverse order so the authority also proves that cascade
history/order does not leak into final computed-style identity.

## Exact denominator

- 32 unique computed styles;
- 8 winning properties per style;
- 7-property common canonical prefix;
- 4096 total intern requests;
- exactly 40 retained DAG nodes after all unique styles are interned:
  root + 7 shared prefix nodes + 32 terminal nodes;
- the remaining 4064 requests must reuse the exact terminal IDs and create zero
  retained nodes.

This is an exact deterministic denominator, not a sample chosen after seeing
results.

## Bound rejection

A second authority run configures a 39-node ceiling. The first 31 unique styles
fit exactly. The 32nd would require node 40 and must fail with
`NodeLimitExceeded`, publish no terminal ID and leave the DAG at exactly 39
logical nodes.

Both runs use the production `ComputedStyle` ledger. The primary reuse run has
a one-mebibyte hard limit and must complete without a rejected reservation.

## Platform authority

The dedicated authority workflow runs on Ubuntu 24.04 and Windows Server 2022.
Both jobs must build and execute the production authority binary and validate
the frozen scope contract.

## Boundary

This authority certifies bounded canonical sharing and exact node-limit
rejection for the production computed-style DAG. It does not claim inheritance,
shorthand expansion, computed-value normalization, invalidation propagation,
offscreen materialization or full browser computed-style semantics.

This authority does not change milestone status by itself. Current Z3 status is owned by `config/zenith_program.json` and the gate-closure certification.
