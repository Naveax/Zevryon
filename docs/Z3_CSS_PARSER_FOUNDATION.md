# Z3 bounded CSS parser foundation

## Production surface

`parse_css_stylesheet_v1` is compiled into `zevryon-massivedoc-core`. It consumes a bounded stylesheet byte view and produces a compact PMR-backed representation made of flat style-rule and declaration records plus one shared text arena. Text fields are represented by 32-bit slices into that arena rather than independent heap strings.

The caller chooses the PMR resource. The focused authority uses `LedgerMemoryResource` charged to `ResourceClass::ComputedStyle`, so real vector/string capacity growth is subject to the existing hard resource ledger rather than an estimated byte counter.

## Foundation grammar

This first Z3 slice accepts qualified style rules with bounded declaration lists. It handles:

- ASCII property identifiers with canonical lowercase storage;
- case-preserving custom property names;
- comments outside quoted strings;
- quoted strings and escaped bytes;
- balanced parentheses and square brackets in selectors;
- balanced parentheses, square brackets and nested value blocks in declarations;
- semicolons inside strings/functions/blocks without terminating the declaration;
- case-insensitive trailing `!important`;
- multiple qualified rules and empty declaration blocks.

The parser is strict at this boundary. Malformed or unsupported syntax returns a classified error and leaves an already-populated output object unchanged.

## Bounds

Configuration is rejected outside hard implementation maxima. The maximum configurable input is 16 MiB, output text arena 128 MiB, rule count 1,048,576, declaration count 4,194,304 and delimiter nesting depth 256. Lower caller-supplied limits are enforced exactly.

Logical output-byte limits and representation-width checks are independent of the PMR hard limit. A `ComputedStyle` ledger rejection surfaces as an allocation failure and candidate allocations unwind without corrupting accounting.

## Deliberate boundary

This slice is foundation work toward `css_parser_conformance`; it does **not** admit that Z3 gate yet. At-rules, full CSS Syntax error recovery/corpus authority, selector matching, cascade/specificity, dependency invalidation, shared computed-style DAGs and offscreen materialization policy remain separate work.

The frozen machine-readable boundary is `certification/z3_css_parser_foundation_scope.json`.
