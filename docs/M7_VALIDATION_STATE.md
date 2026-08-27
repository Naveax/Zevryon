# M7 exact-head validation state

This file records the validation discipline for the integrated competitor executor candidate.

## Current validation rule

The branch must have at most one active `Windows and Linux CI` run for the current exact head SHA. Before any branch write or rerun, inspect queued and in-progress Actions. If an equivalent run already exists, keep its run ID and do not create another one.

A GitHub Actions `startup_failure`, or a run in which no job executes any step, is infrastructure/orchestration evidence only. It is neither a code PASS nor a code FAIL and cannot be used for admission.

The integrated M7 executor is admitted only when one exact-head run completes successfully across the workflow's Linux, Windows, Unicode, Apple-removal, Win32, and i386 gates. The admitted main commit must preserve the exact validated tree.

## Candidate scope

The integrated candidate contains:

- shared Playwright/WebDriver scenario authority;
- Servo and Ladybird exact-binary WebDriver runtime identity;
- W3C synchronous and asynchronous script transport;
- exact `800x720` inner viewport enforcement;
- PID-plus-create-time post-control-baseline browser process-scope memory evidence;
- planner execution for Playwright and WebDriver adapters;
- parent-runner terminal evidence validation;
- dependency-free authority tests for the scenario, runtime, executor, planner, and benchmark runner;
- updated M7 adapter/evidence contract.

## Previous non-admission runs

- `32984872310`: push-triggered run ended without producing usable job execution evidence.
- `32984846149`: pull-request-triggered duplicate surface ended after its jobs were cancelled; no admission evidence.
- `32985816321`: exact-head push run concluded `startup_failure`; its jobs never produced test execution evidence.

None of these runs should be rerun merely to poll CI state. A new run is justified only by a new exact-head candidate or an explicit infrastructure recovery action after confirming there is no equivalent active run.
