# M6 portable scalar exact-match authority and optional SIMD dispatch

## Scope

This slice makes MassiveDoc exact-byte matching explicitly portable: the scalar implementation is the correctness authority and SIMD is an optional candidate prefilter.

## Backends

`ExactByteMatchBackend` exposes the selected process backend:

- `Scalar`
- `Sse2`
- `Neon`

`find_exact_bytes_scalar()` is always available and does not execute SIMD batches. `find_exact_bytes()` selects the process backend once, uses SIMD only when available, and finishes any remainder through the same scalar candidate logic.

## x86 SSE2 runtime selection

On x86-64, SSE2 is part of the architecture baseline.

On 32-bit x86 builds where the SSE2 implementation is compiled, Zevryon checks runtime CPU capability before entering the intrinsic path:

- MSVC: CPUID leaf 1, EDX bit 26.
- GCC/Clang i386: `__builtin_cpu_supports("sse2")`.

This prevents the exact-match dispatcher from treating a compile-time intrinsic implementation as proof that the running 32-bit CPU supports SSE2.

This dispatch does not redefine the compiler's global ISA baseline. A binary deliberately compiled globally for an ISA newer than its target CPU is outside this function's ability to repair.

## ARM NEON

When the target build provides NEON, Zevryon may select the NEON backend. A scalar implementation remains present and authoritative in portable/non-NEON builds. This slice does not claim runtime multi-versioning for an ARM32 binary globally compiled with NEON enabled.

## Correctness contract

SIMD only filters candidate positions using the first and last needle bytes. Full equality is still verified before a match is returned.

The regression suite now checks:

1. selected backend name and SIMD availability agree;
2. scalar never records a SIMD batch;
3. boundary/binary matches agree between auto and scalar paths;
4. a deterministic randomized matrix satisfies `auto == scalar == std::search`;
5. StoreReader exact-search integration still returns the same source record, logical id and byte offset.

No file-format, search-result, or first-match semantics change is admitted by this optimization.
