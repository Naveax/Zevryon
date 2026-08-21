# M6 32-bit address-space window policy

## Scope

This slice makes the existing bounded file-I/O and cold-mapping limits depend on the process address-space width while preserving 64-bit file positions.

## Limits

The native policy is deterministic and can be tested for a synthetic pointer width:

| Address space | Positional I/O hard cap | Cold mapped window hard cap | Materialized-slice policy cap |
| --- | ---: | ---: | ---: |
| 32-bit | 4 MiB | 8 MiB | 8 MiB |
| 64-bit+ | 16 MiB | 16 MiB | 64 MiB |

The default StoreReader I/O window remains 64 KiB on every platform.

`kMaximumIoWindowBytes` and `kMaximumColdMappedWindowBytes` are now derived from the current process address-space policy, so the existing StoreReader and ColdMappedWindow validation paths enforce the narrower 32-bit I/O/mapping bounds without changing file-format limits.

## Offset safety

MassiveDoc file positions remain 64-bit:

- Windows positional reads and mappings split the 64-bit offset into high/low DWORDs.
- POSIX positional I/O requires 64-bit `off_t`.
- File/corpus logical sizes remain `uint64_t` and are not narrowed to `size_t`.

A 4 GiB+ document therefore does not require a 4 GiB virtual mapping.

## Materialization boundary

The policy also exports `maximum_materialized_slice_bytes`. Current Zenith hot-scroll and built-in shared-prefetch requests remain far below it because both use bounded source windows. The general public `StoreReader::read_record_slice(max_bytes)` implementation still needs a dedicated source-level fail-closed check before this M6 item can be considered completely closed for arbitrary callers.

## Validation

`massivedoc-address-space-policy-tests` validates both synthetic 32-bit and 64-bit policies and verifies that StoreReader I/O and cold mmap hard caps are wired to the active policy.
