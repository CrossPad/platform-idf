import http.client, json, sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import catalog_check as cc


def catalog(kits):
    return {"v": 1, "kits": [{"kit": k, "versions": [{"tag": t} for t in tags]} for k, tags in kits.items()]}


MATRIX = json.dumps({"include": [{"rev": "v1", "kit": "full"}, {"rev": "v2", "kit": "full"},
                                 {"rev": "v2", "kit": "lite"}]})


def test_cells_from_the_build_matrix():
    assert cc.cells(MATRIX) == [("v1", "full"), ("v2", "full"), ("v2", "lite")]


def test_all_listed():
    cats = {"v1": catalog({"full": ["v1.1.0-rc3", "v1.0.1"]}),
            "v2": catalog({"full": ["v1.1.0-rc3", "v1.0.1"], "lite": ["v1.1.0-rc3"]})}
    assert cc.missing(cats, "v1.1.0-rc3", cc.cells(MATRIX), "v1.0.1") == []


def test_names_a_missing_kit_and_a_missing_stable():
    cats = {"v1": catalog({"full": ["v1.1.0-rc3"]}), "v2": catalog({"full": ["v1.1.0-rc3", "v1.0.1"]})}
    problems = cc.missing(cats, "v1.1.0-rc3", cc.cells(MATRIX), "v1.0.1")
    assert "v2/lite: v1.1.0-rc3 not listed" in problems
    assert "v1/full: newest stable v1.0.1 not listed" in problems


def test_no_stable_published_checks_only_the_tag():
    cats = {"v1": catalog({"full": ["v1.1.0-rc3"]}), "v2": catalog({"full": ["v1.1.0-rc3"], "lite": ["v1.1.0-rc3"]})}
    assert cc.missing(cats, "v1.1.0-rc3", cc.cells(MATRIX), None) == []


def test_newest_published_stable():
    rels = [{"tag_name": "v1.0.0", "draft": False}, {"tag_name": "v1.0.1", "draft": True},
            {"tag_name": "v1.1.0-rc2", "draft": False}]
    assert cc.newest_published_stable(rels) == "v1.0.0"
    assert cc.newest_published_stable([{"tag_name": "v1.1.0-rc2", "draft": False}]) is None


# --- main: no network, no sleeping, no real alerts -------------------------

class FakeGH:
    def __init__(self, releases):
        self.releases = releases

    def paginate(self, path):
        assert path.startswith("/")
        yield from self.releases


@pytest.fixture
def env(monkeypatch):
    # responses: rev -> catalogs (or exceptions), consumed one per fetch, the last one repeats
    e = SimpleNamespace(alerts=[], fetched=[], sleeps=[], responses={})
    monkeypatch.setattr(cc.gh_api, "from_env", lambda: FakeGH([{"tag_name": "v1.0.1", "draft": False}]))
    monkeypatch.setattr(cc.ci_alert, "send", lambda title, body, **kw: e.alerts.append((title, body)))
    monkeypatch.setattr(cc.time, "sleep", lambda s: e.sleeps.append(s))

    def fake_fetch(rev):
        e.fetched.append(rev)
        queue = e.responses[rev]
        item = queue.pop(0) if len(queue) > 1 else queue[0]
        if isinstance(item, Exception):
            raise item
        return item
    monkeypatch.setattr(cc, "fetch", fake_fetch)
    return e


ARGS = ["--repo", "o/r", "--tag", "v1.1.0-rc3", "--matrix", MATRIX, "--interval", "7"]
GOOD = {"v1": catalog({"full": ["v1.1.0-rc3", "v1.0.1"]}),
        "v2": catalog({"full": ["v1.1.0-rc3", "v1.0.1"], "lite": ["v1.1.0-rc3"]})}


def test_main_passes_when_listed(env, capsys):
    env.responses = {rev: [cat] for rev, cat in GOOD.items()}
    assert cc.main(ARGS) == 0
    assert env.alerts == [] and env.sleeps == []
    assert env.fetched == ["v1", "v2"]      # one fetch per rev, not per kit
    assert "catalog lists v1.1.0-rc3 for 3 cell(s); newest stable: v1.0.1" in capsys.readouterr().out


def test_main_waits_until_the_catalog_catches_up(env):
    late = {"v1": catalog({"full": ["v1.0.1"]}), "v2": GOOD["v2"]}
    env.responses = {"v1": [late["v1"], GOOD["v1"]], "v2": [late["v2"]]}
    assert cc.main(ARGS) == 0
    assert env.sleeps == [7.0]
    assert env.alerts == []


def test_main_retries_after_a_failed_fetch(env):
    env.responses = {"v1": [OSError("down"), GOOD["v1"]], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS) == 0
    assert env.sleeps == [7.0]


def test_main_alerts_and_fails_at_the_deadline(env, capsys):
    env.responses = {"v1": [catalog({"full": ["v1.1.0-rc3"]})], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS + ["--timeout", "0"]) == 1
    assert env.sleeps == []
    assert env.alerts == [("catalog: v1.1.0-rc3", "v1/full: newest stable v1.0.1 not listed")]
    assert "::error::v1/full: newest stable v1.0.1 not listed" in capsys.readouterr().out


def test_main_reports_an_unreachable_catalog(env):
    env.responses = {"v1": [OSError("boom")], "v2": [OSError("boom")]}
    assert cc.main(ARGS + ["--timeout", "0"]) == 1
    assert env.alerts == [("catalog: v1.1.0-rc3", "catalog fetch failed: boom")]


def test_main_treats_a_malformed_body_as_a_failed_fetch(env):
    env.responses = {"v1": [ValueError("not json")], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS + ["--timeout", "0"]) == 1
    assert "catalog fetch failed: not json" in env.alerts[0][1]


def test_main_alerts_at_the_deadline_on_a_null_kits_body(env):
    env.responses = {"v1": [{"v": 1, "kits": None}], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS + ["--timeout", "0"]) == 1
    assert len(env.alerts) == 1 and env.alerts[0][1].startswith("catalog fetch failed: TypeError")


def test_main_retries_after_incomplete_read_then_alerts(env):
    err = http.client.IncompleteRead(b"x")
    env.responses = {"v1": [err], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS + ["--timeout", "0"]) == 1
    assert "IncompleteRead" in env.alerts[0][1]


def test_main_sleep_is_clamped_to_the_remaining_deadline(env, monkeypatch):
    clock = [100.0]
    monkeypatch.setattr(cc.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(cc.time, "sleep", lambda s: (env.sleeps.append(s), clock.__setitem__(0, clock[0] + s)))
    env.responses = {"v1": [OSError("boom")], "v2": [GOOD["v2"]]}
    assert cc.main(ARGS + ["--timeout", "10", "--interval", "7"]) == 1
    assert env.sleeps == [7, 3]


def test_main_alerts_when_the_release_listing_fails(env, monkeypatch, capsys):
    class Boom:
        def paginate(self, path):
            raise cc.gh_api.ApiError(502, "GET", path, "bad gateway")
            yield
    monkeypatch.setattr(cc.gh_api, "from_env", lambda: Boom())
    assert cc.main(ARGS) == 1
    assert len(env.alerts) == 1
    assert env.alerts[0][0] == "catalog: v1.1.0-rc3" and "ApiError" in env.alerts[0][1]
    assert env.fetched == []


# --- fetch: urllib stubbed --------------------------------------------------

class FakeResponse:
    def __init__(self, body):
        self.body = body

    def read(self):
        return self.body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_fetch_asks_crosspad_app_for_the_rev_without_a_token(monkeypatch):
    seen = {}

    def fake_urlopen(req, timeout):
        seen["req"], seen["timeout"] = req, timeout
        return FakeResponse(json.dumps(catalog({"full": ["v1.0.1"]})).encode())
    monkeypatch.setattr(cc.urllib.request, "urlopen", fake_urlopen)
    assert cc.listed(cc.fetch("v2"), "full") == {"v1.0.1"}
    assert seen["req"].full_url == "https://crosspad.app/api/device/catalog?rev=v2"
    assert not any(k.lower() == "authorization" for k in seen["req"].headers)
    assert seen["timeout"] == 20


def test_fetch_rejects_a_body_that_is_not_an_object(monkeypatch):
    monkeypatch.setattr(cc.urllib.request, "urlopen", lambda req, timeout: FakeResponse(b"[1, 2]"))
    with pytest.raises(ValueError):
        cc.fetch("v2")
