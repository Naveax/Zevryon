# M5 — Unbounded Controller Scaling

`ZenithProcessTabController` must not turn an unbounded tab policy into quadratic bookkeeping cost.

The first controller slice recomputed `visible_tabs` and `hidden_tabs` by walking the full registry after every registration, unregistration, and visibility update. That is correct at small cardinalities but makes opening N tabs O(N²), which is the wrong scaling contract for a browser with no finite tab-count policy.

This follow-up keeps visibility counts incrementally:

- registration updates exactly one counter;
- unregistration updates exactly one counter;
- hidden/visible transitions move one count between the two classes;
- telemetry snapshots copy the already-maintained counts in O(1);
- no full registry scan is performed for ordinary tab lifecycle bookkeeping.

The only intentional full-registry work left in the controller is a global pressure transition, because every registered tab must actually receive the new pressure policy. Hidden tabs are still applied before visible tabs.

The focused C++20 harness was rebuilt with `-Wall -Wextra -Wpedantic -Werror` after this change and all controller behavior tests passed.
