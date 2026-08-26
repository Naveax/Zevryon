#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from browser_competitor_registry import CompetitorSpec, get_spec


@dataclass(frozen=True)
class PlaywrightLaunchPlan:
    competitor: str
    canonical_name: str
    browser_type: str
    channel: str | None
    distribution: str
    args: tuple[str, ...]


def launch_plan(competitor: str) -> PlaywrightLaunchPlan:
    spec = get_spec(competitor)
    if spec.adapter != "playwright" or spec.playwright_browser is None:
        raise ValueError(
            f"competitor is not supported by the Playwright adapter: {competitor}"
        )

    args: tuple[str, ...] = ()
    if spec.playwright_browser == "chromium":
        args = ("--js-flags=--expose-gc",)

    distribution = (
        "branded-channel"
        if spec.playwright_channel is not None
        else "playwright-managed"
    )
    return PlaywrightLaunchPlan(
        competitor=spec.key,
        canonical_name=spec.canonical_name,
        browser_type=spec.playwright_browser,
        channel=spec.playwright_channel,
        distribution=distribution,
        args=args,
    )


def launch_browser(
    playwright: Any,
    competitor: str,
) -> tuple[CompetitorSpec, Any, PlaywrightLaunchPlan]:
    spec = get_spec(competitor)
    plan = launch_plan(competitor)
    try:
        browser_type = getattr(playwright, plan.browser_type)
    except AttributeError as exc:
        raise ValueError(
            f"Playwright runtime lacks browser type {plan.browser_type}: {competitor}"
        ) from exc

    kwargs: dict[str, object] = {"headless": True}
    if plan.channel is not None:
        kwargs["channel"] = plan.channel
    if plan.args:
        kwargs["args"] = list(plan.args)
    browser = browser_type.launch(**kwargs)
    return spec, browser, plan


def runtime_identity(
    spec: CompetitorSpec,
    plan: PlaywrightLaunchPlan,
    browser_version: str,
) -> str:
    version = browser_version.strip()
    if not version:
        raise ValueError("Playwright browser runtime identity requires a version")
    if spec.key != plan.competitor:
        raise ValueError("Playwright launch plan competitor does not match registry spec")
    if spec.canonical_name != plan.canonical_name:
        raise ValueError("Playwright launch plan canonical name does not match registry spec")

    channel = plan.channel if plan.channel is not None else "none"
    return (
        f"{spec.canonical_name}; adapter=playwright; "
        f"browser_type={plan.browser_type}; channel={channel}; "
        f"distribution={plan.distribution}; version={version}"
    )
