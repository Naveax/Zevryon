# Security Policy

Zevryon is an experimental independent browser-engine project. Do not report a
potential vulnerability in a public issue before coordinated disclosure.

## Supported versions

Only the newest tagged development release receives security fixes.

## Reporting

Until a dedicated security mailbox and private advisory channel are configured,
use a private GitHub Security Advisory in the project repository. Include:

- affected commit/version;
- minimal reproduction;
- expected security boundary;
- actual result;
- platform and toolchain;
- whether the issue is already public.

No release may claim a hardened security boundary before renderer sandboxing,
IPC capability validation, fuzzing and independent review are complete.
