from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
from unittest import mock

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "prepare_m5_physical_frame.py"
SPEC = importlib.util.spec_from_file_location("prepare_m5_physical_frame", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def test_physical_environment_is_fail_closed() -> None:
    with mock.patch.dict(os.environ, {}, clear=True):
        with pytest.raises(ValueError, match="ZEVRYON_PHYSICAL_DEVICE"):
            module._validate_environment()

    with mock.patch.dict(
        os.environ,
        {"ZEVRYON_PHYSICAL_DEVICE": "1", "ZEVRYON_THERMAL_STATE": "serious"},
        clear=True,
    ):
        with pytest.raises(ValueError, match="nominal or fair"):
            module._validate_environment()

    with mock.patch.dict(
        os.environ,
        {"ZEVRYON_PHYSICAL_DEVICE": "1", "ZEVRYON_THERMAL_STATE": "nominal"},
        clear=True,
    ):
        module._validate_environment()


def test_resolve_executable_supports_windows_and_single_config_layouts() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        build = Path(temporary)
        release = build / "Release"
        release.mkdir()
        windows_binary = release / "zevryon-massivedoc.exe"
        windows_binary.write_bytes(b"x")
        assert module._resolve_executable(build, "zevryon-massivedoc") == windows_binary.resolve()

    with tempfile.TemporaryDirectory() as temporary:
        build = Path(temporary)
        linux_binary = build / "zevryon-zenith-frame-probe"
        linux_binary.write_bytes(b"x")
        assert module._resolve_executable(build, "zevryon-zenith-frame-probe") == linux_binary.resolve()


def test_last_json_accepts_pretty_or_final_json_line() -> None:
    assert module._last_json('{"a":1}') == {"a": 1}
    assert module._last_json('noise\n{"b":2}\n') == {"b": 2}
    with pytest.raises(ValueError):
        module._last_json("noise only")


def test_safe_reset_refuses_repository_root_and_clears_normal_workdir() -> None:
    with pytest.raises(ValueError, match="unsafe work directory"):
        module._safe_reset_work_dir(module.ROOT)

    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary) / "work"
        work.mkdir()
        stale = work / "stale.txt"
        stale.write_text("stale", encoding="utf-8")
        module._safe_reset_work_dir(work)
        assert work.is_dir()
        assert not stale.exists()


def test_sample_floor_matches_physical_gate() -> None:
    assert module._positive("samples", 1000) == 1000
    with pytest.raises(ValueError):
        module._positive("samples", 0)
