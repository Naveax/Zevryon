# M6 32-bit address-space window policy

## Scope

This slice makes the existing bounded file-I/O, cold-mapping and record-slice materialization limits depend on the process address-space width while preserving 64-bit file positions.

## Limits

The native policy is deterministic and can be tested for a synthetic pointer width:

| Address space | Positional I/O hard cap | Cold mapped window hard cap | Materialized record-slice hard cap |
| --- | ---: | ---: | ---: |
| 32-bit | 4 MiB | 8 MiB | 8 MiB |
| 64-bit+ | 16 MiB | 16 MiB | 64 MiB |

The default StoreReader I/O window remains 64 KiB on every platform.

`kMaximumIoWindowBytes` and `kMaximumColdMappedWindowBytes` are derived from the current process address-space policy, so the existing StoreReader and ColdMappedWindow validation paths enforce the narrower 32-bit I/O/mapping bounds without changing file-format limits.

`StoreReader::read_record_slice()` computes the actual requested materialization as `min(record_remaining, max_bytes)` and rejects it before `vector::reserve()` when that materialization exceeds the current address-space cap. A caller may therefore provide a very large upper bound without being rejected when only a small tail actually remains.

The same entry point now rejects null output/error pointers before dereference.

## Offset safety

MassiveDoc file positions remain 64-bit:

- Windows positional reads and mappings split the 64-bit offset into high/low DWORDs.
- Linux compiles the entire `zevryon-massivedoc-core` target with `_FILE_OFFSET_BITS=64`; `massivedoc_large_file_abi_guard.cpp` fails compilation if that definition is absent, not equal to 64, or still produces an `off_t` narrower than 64 bits.
- File/corpus logical sizes remain `uint64_t` and are not narrowed to `size_t`.

A 4 GiB+ document therefore does not require a 4 GiB virtual mapping or a giant contiguous record-slice allocation.

## Validation

`massivedoc-address-space-policy-tests` validates synthetic 32-bit and 64-bit limits, including exact-boundary acceptance and one-byte-over rejection, and verifies that StoreReader I/O and cold mmap hard caps are wired to the active process policy. On an actual 32-bit process build the same test additionally requires the active 4/8/8 MiB policy.

The Windows/Linux CI workflow contains two dedicated real-ABI gates:

- `M6 Win32 address-space gate` configures Visual Studio 2022 with `-A Win32`, builds the MassiveDoc core plus the address-space, positional-I/O, positional-store and cold-window tests, and runs those focused tests under the real 32-bit MSVC ABI.
- `M6 Linux i386 address-space gate` installs the multilib compiler, compiles a `-m32` ABI probe that requires 32-bit pointers plus 64-bit `off_t`, and syntax-compiles the real positional-I/O and cold-window source files with the same large-file ABI definition.

These are separate from the normal x64 Windows/Linux jobs and prevent a synthetic pointer-width test from being the only 32-bit evidence.

Zenith hot-scroll and built-in shared prefetch remain substantially below these limits because their source windows are bounded to the normal 64 KiB I/O window.
