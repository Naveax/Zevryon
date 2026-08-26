#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_process_scope import (
    BrowserProcessScopeMonitor,
    ProcessIdentity,
    dynamic_scope_identities,
    process_set_memory_bytes,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    driver_a = ProcessIdentity(10, 100)
    driver_b = ProcessIdentity(11, 110)
    browser_a = ProcessIdentity(20, 200)
    browser_b = ProcessIdentity(21, 210)
    browser_a_reused = ProcessIdentity(20, 999)
    baseline = frozenset({driver_a, driver_b})

    current = frozenset({driver_a, driver_b, browser_a, browser_b})
    require(
        dynamic_scope_identities(1, baseline, identity_provider=lambda _root: current)
        == frozenset({browser_a, browser_b}),
        "post-baseline process scope drift",
    )

    memory = {browser_a: 100, browser_b: 200}
    require(
        process_set_memory_bytes(
            [browser_a, browser_b, browser_b],
            memory_reader=lambda identity: memory[identity],
        )
        == 300,
        "process-set memory double-counted or omitted an identity",
    )

    states = iter(
        [
            frozenset({driver_a, driver_b, browser_a}),
            frozenset({driver_a, driver_b, browser_a, browser_b}),
            frozenset({driver_a, driver_b, browser_a_reused, browser_b}),
        ]
    )
    last = frozenset({driver_a, driver_b, browser_a_reused, browser_b})

    def provider(_root: int) -> frozenset[ProcessIdentity]:
        nonlocal last
        try:
            last = next(states)
        except StopIteration:
            pass
        return last

    monitor = BrowserProcessScopeMonitor(
        1,
        baseline,
        identity_provider=provider,
        memory_reader=lambda identity: {
            browser_a: 400,
            browser_b: 700,
            browser_a_reused: 50,
        }.get(identity, 0),
        interval_seconds=1.0,
    )
    first = monitor.snapshot()
    second = monitor.snapshot()
    final = monitor.stop()

    require(first.identities == (browser_a,), "first browser identity scope drift")
    require(
        second.identities == (browser_a, browser_b),
        "dynamic browser child was not admitted",
    )
    require(
        final.identities == (browser_a_reused, browser_b),
        "PID reuse was not represented as a distinct process identity",
    )
    require(monitor.peak_bytes == 1100, "browser-scope peak memory drift")
    require(
        monitor.observed_identities == {browser_a, browser_b, browser_a_reused},
        "observed process identity receipt drift",
    )
    require(monitor.valid, "non-empty browser scope was marked invalid")
    require(
        monitor.observed_receipts
        == [
            {"pid": 20, "create_time_ns": 200},
            {"pid": 20, "create_time_ns": 999},
            {"pid": 21, "create_time_ns": 210},
        ],
        "process identity receipts are not stable and sorted",
    )

    empty = BrowserProcessScopeMonitor(
        1,
        baseline,
        identity_provider=lambda _root: baseline,
        memory_reader=lambda _identity: 0,
        interval_seconds=1.0,
    )
    empty.stop()
    require(not empty.valid, "empty browser scope was marked valid")

    duplicate_start = BrowserProcessScopeMonitor(
        1,
        baseline,
        identity_provider=lambda _root: baseline,
        memory_reader=lambda _identity: 0,
        interval_seconds=1.0,
    )
    duplicate_start.start()
    try:
        duplicate_start.start()
    except RuntimeError:
        pass
    else:
        raise AssertionError("duplicate monitor start was accepted")
    finally:
        duplicate_start.stop()

    print("Zevryon competitor process-scope tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
