# M5 — Frame-Profile Scheduler Certification

This certification locks the native device budgets to the scheduler behavior that M5 requires, not merely to constants in a lookup table.

For every canonical device profile (`legacy-phone`, `mid-phone`, `modern-phone`, `desktop`) the focused test proves:

- speculative prefetch is rejected while the visible phase is still open;
- visible UI work is admitted first;
- prefetch is admitted only after `finish_visible_phase()` and only within that profile's optional budget;
- elevated pressure halves the optional prefetch cap;
- critical pressure preserves visible work while suppressing speculative work;
- hidden tabs reject frame work and expose zero remaining frame budget.

This test does not certify wall-clock performance on physical hardware. It certifies scheduler policy and ordering. Physical-device benchmark metadata and measured P99/P95 results remain separate evidence gates.
