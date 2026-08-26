#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_playwright import launch_browser, launch_plan, runtime_identity
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
    require(chrome.channel == "chrome", "Chrome launch channel mismatch")
    require(edge.channel == "msedge", "Edge launch channel mismatch")
    require(firefox.browser_type == "firefox", "Firefox launch browser mismatch")
    require(webkit.browser_type == "webkit", "WebKit launch browser mismatch")
    require(not firefox.args and not webkit.args, "non-Chromium launch received Chromium args")
    require(
        "--js-flags=--expose-gc" in chrome.args and
        "--js-flags=--expose-gc" in edge.args,
        "branded Chromium launch lost deterministic GC exposure argument",
    )

    require_value_error(
        lambda: launch_plan("servo"),
        "Servo was incorrectly routed through the Playwright adapter",
    )
    require_value_error(
        lambda: launch_plan("ladybird"),
        "Ladybird was incorrectly routed through the Playwright adapter",
    )

    fake = FakePlaywright()
    chrome_spec, chrome_browser, chrome_plan = launch_browser(fake, "chrome")
    require(chrome_browser.version == "123.4-test", "fake Chrome launch did not return browser")
    require(len(fake.chromium.calls) == 1, "Chrome did not launch Chromium browser type exactly once")
    chrome_call = fake.chromium.calls[0]
    require(chrome_call.get("headless") is True, "Chrome launch was not headless")
    require(chrome_call.get("channel") == "chrome", "Chrome launch did not preserve exact channel")
    require(
        chrome_call.get("args") == ["--js-flags=--expose-gc"],
        "Chrome launch args mismatch",
    )

    identity = runtime_identity(chrome_spec, chrome_plan, chrome_browser.version)
    require("Google Chrome" in identity, "runtime identity lost canonical Chrome name")
    require("channel=chrome" in identity, "runtime identity lost Chrome channel")
    require("version=123.4-test" in identity, "runtime identity lost browser version")

    firefox_spec, firefox_browser, firefox_plan = launch_browser(fake, "firefox")
    require(len(fake.firefox.calls) == 1, "Firefox did not launch exactly once")
    firefox_call = fake.firefox.calls[0]
    require("channel" not in firefox_call, "Firefox launch fabricated a branded channel")
    require("args" not in firefox_call, "Firefox launch received Chromium-only args")
    firefox_identity = runtime_identity(
        firefox_spec, firefox_plan, firefox_browser.version
    )
    require("browser_type=firefox" in firefox_identity, "Firefox identity lost browser type")
    require("channel=bundled" in firefox_identity, "Firefox identity did not mark bundled runtime")

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
