#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_playwright import (
    launch_browser,
    launch_failure_status,
    launch_plan,
    runtime_identity,
)
from browser_competitor_registry import get_spec


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


class FakeBrowser:
    version = "123.4-test"


class FakeBrowserType:
    def __init__(self) -> None:
        self.calls: list[dict[str, object]] = []

    def launch(self, **kwargs: object) -> FakeBrowser:
        self.calls.append(kwargs)
        return FakeBrowser()


class FakePlaywright:
    def __init__(self) -> None:
        self.chromium = FakeBrowserType()
        self.firefox = FakeBrowserType()
        self.webkit = FakeBrowserType()


def main() -> int:
    chromium = launch_plan("chromium")
    chrome = launch_plan("chrome")
    edge = launch_plan("edge")
    firefox = launch_plan("firefox")
    webkit = launch_plan("webkit")

    require(chromium.browser_type == "chromium", "Chromium browser type mismatch")
    require(chromium.channel is None, "auxiliary Chromium unexpectedly has a channel")
    require(
        chromium.distribution == "playwright-managed",
        "auxiliary Chromium distribution identity mismatch",
    )
    require(chrome.channel == "chrome", "Chrome launch channel mismatch")
    require(chrome.distribution == "branded-channel", "Chrome distribution mismatch")
    require(edge.channel == "msedge", "Edge launch channel mismatch")
    require(edge.distribution == "branded-channel", "Edge distribution mismatch")
    require(firefox.browser_type == "firefox", "Firefox launch browser mismatch")
    require(
        firefox.distribution == "playwright-managed",
        "Firefox distribution identity mismatch",
    )
    require(webkit.browser_type == "webkit", "WebKit launch browser mismatch")
    require(
        webkit.distribution == "playwright-managed",
        "WebKit distribution identity mismatch",
    )
    require(not firefox.args and not webkit.args, "non-Chromium launch received Chromium args")
    require(
        "--js-flags=--expose-gc" in chromium.args
        and "--js-flags=--expose-gc" in chrome.args
        and "--js-flags=--expose-gc" in edge.args,
        "Chromium-family launch lost deterministic GC exposure argument",
    )

    require_value_error(
        lambda: launch_plan("servo"),
        "Servo was incorrectly routed through the Playwright adapter",
    )
    require_value_error(
        lambda: launch_plan("ladybird"),
        "Ladybird was incorrectly routed through the Playwright adapter",
    )
    require_value_error(
        lambda: launch_browser(object(), "chrome"),
        "missing Playwright browser type did not fail closed",
    )

    fake = FakePlaywright()

    chromium_spec, chromium_browser, chromium_plan = launch_browser(fake, "chromium")
    require(len(fake.chromium.calls) == 1, "auxiliary Chromium did not launch exactly once")
    require(
        fake.chromium.calls[-1]
        == {"headless": True, "args": ["--js-flags=--expose-gc"]},
        "auxiliary Chromium launch kwargs mismatch",
    )
    chromium_identity = runtime_identity(
        chromium_spec, chromium_plan, chromium_browser.version
    )
    require("channel=none" in chromium_identity, "Chromium identity fabricated a channel")
    require(
        "distribution=playwright-managed" in chromium_identity,
        "Chromium identity lost Playwright-managed distribution",
    )

    chrome_spec, chrome_browser, chrome_plan = launch_browser(fake, "chrome")
    require(len(fake.chromium.calls) == 2, "Chrome did not launch Chromium browser type once")
    require(
        fake.chromium.calls[-1]
        == {
            "headless": True,
            "channel": "chrome",
            "args": ["--js-flags=--expose-gc"],
        },
        "Chrome launch kwargs mismatch",
    )
    chrome_identity = runtime_identity(chrome_spec, chrome_plan, chrome_browser.version)
    require("Google Chrome" in chrome_identity, "runtime identity lost canonical Chrome name")
    require("channel=chrome" in chrome_identity, "runtime identity lost Chrome channel")
    require(
        "distribution=branded-channel" in chrome_identity,
        "Chrome identity lost branded distribution",
    )
    require("version=123.4-test" in chrome_identity, "runtime identity lost browser version")

    edge_spec, edge_browser, edge_plan = launch_browser(fake, "edge")
    require(len(fake.chromium.calls) == 3, "Edge did not launch Chromium browser type once")
    require(
        fake.chromium.calls[-1]
        == {
            "headless": True,
            "channel": "msedge",
            "args": ["--js-flags=--expose-gc"],
        },
        "Edge launch kwargs mismatch",
    )
    edge_identity = runtime_identity(edge_spec, edge_plan, edge_browser.version)
    require("Microsoft Edge" in edge_identity, "runtime identity lost canonical Edge name")
    require("channel=msedge" in edge_identity, "runtime identity lost Edge channel")

    firefox_spec, firefox_browser, firefox_plan = launch_browser(fake, "firefox")
    require(len(fake.firefox.calls) == 1, "Firefox did not launch exactly once")
    require(
        fake.firefox.calls[-1] == {"headless": True},
        "Firefox launch fabricated channel or Chromium args",
    )
    firefox_identity = runtime_identity(
        firefox_spec, firefox_plan, firefox_browser.version
    )
    require("browser_type=firefox" in firefox_identity, "Firefox identity lost browser type")
    require("channel=none" in firefox_identity, "Firefox identity fabricated a channel")
    require(
        "distribution=playwright-managed" in firefox_identity,
        "Firefox identity lost Playwright-managed distribution",
    )

    webkit_spec, webkit_browser, webkit_plan = launch_browser(fake, "webkit")
    require(len(fake.webkit.calls) == 1, "WebKit did not launch exactly once")
    require(
        fake.webkit.calls[-1] == {"headless": True},
        "WebKit launch fabricated channel or Chromium args",
    )
    webkit_identity = runtime_identity(webkit_spec, webkit_plan, webkit_browser.version)
    require("browser_type=webkit" in webkit_identity, "WebKit identity lost browser type")
    require("channel=none" in webkit_identity, "WebKit identity fabricated a channel")
    require(
        "distribution=playwright-managed" in webkit_identity,
        "WebKit identity lost Playwright-managed distribution",
    )

    unavailable_status, unavailable_reason = launch_failure_status(
        RuntimeError(
            "BrowserType.launch: Executable doesn't exist at /tmp/chromium; "
            "Please run Playwright install"
        )
    )
    require(unavailable_status == "unavailable", "missing executable was not unavailable")
    require("Executable doesn't exist" in unavailable_reason, "unavailable reason lost detail")

    channel_status, _ = launch_failure_status(
        RuntimeError("Browser distribution 'chrome' is not found")
    )
    require(channel_status == "unavailable", "missing branded channel was not unavailable")

    generic_status, generic_reason = launch_failure_status(
        RuntimeError("sandbox initialization failed")
    )
    require(generic_status == "error", "generic launch failure was mislabeled unavailable")
    require("sandbox initialization failed" in generic_reason, "generic failure reason lost detail")

    require_value_error(
        lambda: runtime_identity(get_spec("chrome"), edge, "123"),
        "mismatched registry spec and launch plan were accepted",
    )
    require_value_error(
        lambda: runtime_identity(get_spec("webkit"), webkit, "   "),
        "blank browser version was accepted",
    )

    print("Zevryon Playwright competitor adapter tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
