# Z3 CSS parser, cascade and style-sharing milestone certification

## Status

`config/zenith_program.json` marks Z3 `implemented` only after all five configured gates have canonical passing authority.

| Required gate | Canonical authority | Result |
|---|---|---|
| `css_parser_conformance` | `z3-css-parser-conformance-v1` | PASS on Linux/Windows configured parser corpus and frozen recovery/preprocessing authority |
| `cascade_conformance` | `zevryon.z3-cascade-authority-scope.v1` | PASS for configured author-origin selector/cascade precedence authority |
| `selector_dependency_invalidation` | `zevryon.z3-selector-invalidation-authority-scope.v1` | PASS for bounded selector dependency invalidation authority |
| `bounded_style_dag` | `zevryon.z3-style-dag-authority-scope.v1` | PASS for bounded canonical computed-style DAG sharing authority |
| `offscreen_materialization_bound` | `zevryon.z3-offscreen-materialization-authority-scope.v1` | PASS for bounded offscreen materialization/window authority |

## Immutable CI evidence

- CSS parser conformance: Actions run `35615407455`, exact head `610f2d10d21d8bbdb3c20fe9323b7509760cabb9`, admitted by merge `422876b6eda538ccd2b8a008a81eb2fd45d393ad`;
- author cascade conformance: Actions run `35596502764`, exact head `de9a75a0ac41a388c28a37c892ac31e589107523`, admitted by merge `0638d7d5d749a08b546d0e14a1216045afe1b89c`;
- selector dependency invalidation: Actions run `35596466760`, exact head `ac09640c5f1000a86532005fd0ebfbd7b7bd05a2`, admitted by merge `f270f1260e7b44cd966f3d352d0765261f971b54`;
- bounded style DAG: Actions run `35596454060`, exact head `714792719c881c23877a9b8bc3d76331c51b9aa4`, admitted by merge `f9619e9df3afb338f853ae047fc66f64aecce640`;
- offscreen materialization bound: Actions run `35735167445`, exact head `f92bb11ae77dfe8a976d7a513489bff831b5d7eb`, admitted by merge `e3de35c8c68835f469376d2c9fab6479736d5727`.

The gate-closure manifest is `certification/z3_gate_closure.json`. Gate authorities remain separately scoped and do not independently own milestone promotion.

## Integrated style path at closure

Before final promotion, main also carries the bounded semantic-window style bridge, native standalone declaration-list parser, inline author-cascade merge, and HTML `style=""` integration. These integration slices consume the admitted authorities but are not extra required gates and do not inflate the five-gate closure contract.

## Claim boundary

Z3 completion means the five gates configured in `config/zenith_program.json` have canonical passing authority and the bounded production surfaces are integrated. It is not a claim of universal CSS/WPT/CSSOM conformance, full property grammar or computed-value semantics, inheritance, unsupported selector profiles, or layout/paint completion.
