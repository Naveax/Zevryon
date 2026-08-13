# Z2F macOS Removal Promotion Evidence

## Admission scope

This record promotes the intentional removal of macOS support from Zevryon while preserving Windows and Linux desktop support.

- Base `main`: `a55609a7a1bb1a20a15ffe98c5626374bf9fa9ef`
- Source authority head: `010e904697d66b617dfa3d9cd49255b225df91c5`
- Source authority divergence: 36 commits ahead, 0 behind
- Supported desktop targets: Windows, Linux
- Unsupported desktop target: macOS

The retained `Metal` and `CocoaLayer` enum/type/factory identities are compatibility identities only. They do not advertise an operational Metal or macOS backend. The active native platform adapter rejects Metal and reports zero default Metal capability limits.

## Source exact-head CI receipt

GitHub Actions workflow `Windows and Linux CI`, run `31716197258`, executed on source authority head `010e904697d66b617dfa3d9cd49255b225df91c5` and completed successfully.

Admitted jobs:

- Windows build and headless tests: job `94501582527`, success.
- Linux build and headless tests: job `94501582907`, success.
- Apple backend removal guard: job `94501582633`, success.

The source run proves Windows and Linux configure/build/headless-test viability and the absence of active Apple backend build surface on that exact source authority head.

## Intentional removals

The promotion manifest records the exact 21 paths intentionally removed from the base-main tree. They cover the concrete CoreText discovery implementation/tests and Metal window, GPU SDK, shader execution, swapchain, benchmark, and related documentation surfaces.

No Objective-C or Objective-C++ backend implementation is admitted by this promotion.

## Diff boundary

`certification/z2f_macos_removal_promotion.json` records the complete admitted base-to-promotion changed-path set. Any merge-readiness review must compare the current branch against the frozen base-main SHA and reject an unlisted changed or removed path.

This evidence record intentionally does not embed its own containing commit SHA. Doing so would create a self-referential hash fixed-point problem. The containing exact HEAD must instead be certified by the GitHub Actions run triggered after this evidence commit.

## Promotion conditions

Promotion is admissible only when all of the following remain true:

1. `main` is still the frozen base or the branch is explicitly revalidated against a newer base.
2. The source authority head is an ancestor of the promotion head.
3. The base-to-promotion diff contains only the manifest-admitted paths.
4. Every manifest-declared Apple/Metal/CoreText removal remains absent.
5. `scripts/check_no_apple_backends.py` passes.
6. Windows build and headless tests pass on the containing exact HEAD.
7. Linux build and headless tests pass on the containing exact HEAD.
8. The Apple backend removal guard passes on the containing exact HEAD.

Until those containing-head checks are green, this document is evidence material, not a merge authorization.
