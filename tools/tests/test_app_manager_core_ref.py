"""CROSSPAD_APPS_CORE_REF: the pinned crosspad-apps commit of the manager core.

tools/app_manager.py downloads and loads the core on import, so these tests
exec the source up to that line and drive _ensure_core() with the network
stubbed out.
"""
import types
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[1] / "app_manager.py"
SHA = "0123456789abcdef" * 2 + "01234567"
CORE_TEXT = "class AppManager:\n    pass\n"


@pytest.fixture
def am(tmp_path, monkeypatch):
    monkeypatch.delenv("CROSSPAD_APPS_CORE_REF", raising=False)
    monkeypatch.delenv("CROSSPAD_MANAGER_REFRESH", raising=False)
    head = SRC.read_text(encoding="utf-8").split("\ncore = _load_core()\n")[0]
    mod = types.ModuleType("app_manager_under_test")
    mod.__file__ = str(tmp_path / "app_manager.py")
    exec(compile(head, str(SRC), "exec"), mod.__dict__)
    mod.fetched = []

    def fake_fetch(ref=""):
        mod.fetched.append(ref)
        return CORE_TEXT

    mod._fetch_core = fake_fetch
    return mod


@pytest.mark.parametrize("value", [
    "",                      # present but empty: the file was empty or missing
    SHA[:12],                # short sha
    "main",                  # branch name
    SHA.upper(),             # not lowercase hex
    SHA + "\n",              # stray line
    SHA + "0",               # too long
    "../" + SHA[:37],        # would escape the URL path
])
def test_malformed_ref_is_fatal(am, monkeypatch, capsys, value):
    monkeypatch.setenv("CROSSPAD_APPS_CORE_REF", value)
    with pytest.raises(SystemExit) as e:
        am._ensure_core()
    assert e.value.code == 1
    assert "CROSSPAD_APPS_CORE_REF" in capsys.readouterr().out
    assert am.fetched == []
    assert not am.CORE_CACHE.exists()


def test_valid_ref_downloads_that_commit_every_run(am, monkeypatch):
    monkeypatch.setenv("CROSSPAD_APPS_CORE_REF", SHA)
    am._ensure_core()
    am._ensure_core()  # a fresh cache does not short-circuit a pinned run
    assert am.fetched == [SHA, SHA]
    assert am.CORE_CACHE.read_text(encoding="utf-8") == CORE_TEXT
    assert am.CORE_STAMP.read_text().strip() == am._digest(am.CORE_CACHE)


def test_valid_ref_failed_download_is_fatal(am, monkeypatch, capsys):
    monkeypatch.setenv("CROSSPAD_APPS_CORE_REF", SHA)
    am._fetch_core = lambda ref="": None
    with pytest.raises(SystemExit) as e:
        am._ensure_core()
    assert e.value.code == 1
    assert SHA[:12] in capsys.readouterr().out


def test_unset_keeps_the_unpinned_path(am):
    am._ensure_core()
    assert am.fetched == [""]
    am._ensure_core()  # within the TTL: the cached copy is used
    assert am.fetched == [""]


def test_core_url_follows_the_ref(am):
    assert am._core_url(SHA).endswith(f"/crosspad-apps/{SHA}/crosspad_app_manager.py")
    assert "/crosspad-apps/main/" in am._core_url("")
