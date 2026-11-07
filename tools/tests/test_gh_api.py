import email.message, io, json, sys, urllib.error, urllib.request, urllib.response
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gh_api


class Resp:
    def __init__(self, body, link=""):
        self._body = json.dumps(body).encode() if body is not None else b""
        self.headers = {"Link": link} if link else {}

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def opener_for(pages):
    """pages: {url: Resp | Exception}; records the requests."""
    seen = []

    def _open(req, timeout=None):
        seen.append((req.get_method(), req.full_url, req.data, dict(req.header_items())))
        out = pages[req.full_url]
        if isinstance(out, Exception):
            raise out
        return out
    return _open, seen


def test_get_sends_auth_and_api_version():
    op, seen = opener_for({"https://api.github.com/repos/o/r": Resp({"ok": 1})})
    gh = gh_api.GitHub("T", opener=op)
    assert gh.get("/repos/o/r") == {"ok": 1}
    assert seen[0][0] == "GET"
    headers = {k.lower(): v for k, v in seen[0][3].items()}
    assert headers["authorization"] == "Bearer T"
    assert headers["x-github-api-version"] == "2022-11-28"


def test_paginate_follows_the_next_link():
    first = "https://api.github.com/repos/o/r/releases?per_page=100"
    second = "https://api.github.com/repos/o/r/releases?per_page=100&page=2"
    op, _ = opener_for({first: Resp([{"n": 1}], link=f'<{second}>; rel="next"'),
                        second: Resp([{"n": 2}])})
    assert [x["n"] for x in gh_api.GitHub("T", opener=op).paginate("/repos/o/r/releases")] == [1, 2]


def test_http_error_becomes_api_error():
    url = "https://api.github.com/repos/o/r"
    err = urllib.error.HTTPError(url, 404, "nf", {}, io.BytesIO(b'{"message":"Not Found"}'))
    op, _ = opener_for({url: err})
    with pytest.raises(gh_api.ApiError) as e:
        gh_api.GitHub("T", opener=op).get("/repos/o/r")
    assert e.value.status == 404


def test_post_sends_json():
    op, seen = opener_for({"https://api.github.com/x": Resp({"id": 7})})
    assert gh_api.GitHub("T", opener=op).post("/x", {"a": 1}) == {"id": 7}
    assert seen[0][0] == "POST" and json.loads(seen[0][2]) == {"a": 1}


def test_graphql_errors_raise():
    op, _ = opener_for({"https://api.github.com/graphql": Resp({"errors": [{"message": "no"}]})})
    with pytest.raises(gh_api.ApiError):
        gh_api.GitHub("T", opener=op).graphql("query { x }")


def test_from_env_reads_the_named_variable(monkeypatch):
    monkeypatch.setenv("MY_TOKEN", "abc")
    assert gh_api.from_env("MY_TOKEN").token == "abc"


def test_from_env_without_the_variable_exits(monkeypatch):
    monkeypatch.delenv("GH_TOKEN", raising=False)
    with pytest.raises(SystemExit):
        gh_api.from_env()


def test_foreign_absolute_url_is_refused_without_a_request():
    op, seen = opener_for({})
    gh = gh_api.GitHub("SECRET", opener=op)
    for url in ("https://evil.example/x", "https://api.github.com.evil.example/x",
                "https://api.github.com@evil.example/x", "http://api.github.com/x", "repos/o/r"):
        with pytest.raises(ValueError) as e:
            gh.get(url)
        assert "SECRET" not in str(e.value)
    assert seen == []


def test_paginate_refuses_a_foreign_next_link():
    first = "https://api.github.com/x?per_page=100"
    op, seen = opener_for({first: Resp([{"n": 1}], link='<https://evil.example/p2>; rel="next"')})
    it = gh_api.GitHub("T", opener=op).paginate("/x")
    assert next(it) == {"n": 1}
    with pytest.raises(ValueError):
        next(it)
    assert [u for _, u, _, _ in seen] == [first]


class FakeHTTPS(urllib.request.HTTPSHandler):
    """First hop answers 302 to `target`; records the headers each hop was sent."""

    def __init__(self, target):
        super().__init__()
        self.target, self.hops = target, []

    def https_open(self, req):
        self.hops.append((req.full_url, {k.lower(): v for k, v in req.header_items()}))
        hdrs = email.message.Message()
        if len(self.hops) == 1:
            hdrs["Location"] = self.target
            resp = urllib.response.addinfourl(io.BytesIO(b""), hdrs, req.full_url, 302)
            resp.msg = "Found"
        else:
            resp = urllib.response.addinfourl(io.BytesIO(b"{}"), hdrs, req.full_url, 200)
            resp.msg = "OK"
        return resp


def follow_redirect(target):
    fake = FakeHTTPS(target)
    opener = urllib.request.build_opener(gh_api._DropAuthOnForeignRedirect, fake)
    gh_api.GitHub("SECRET", opener=opener.open).get("/repos/o/r/releases/assets/1")
    return fake.hops


def test_cross_host_redirect_does_not_carry_the_token():
    hops = follow_redirect("https://objects.example/blob")
    assert hops[0][1]["authorization"] == "Bearer SECRET"
    assert hops[1][0] == "https://objects.example/blob"
    assert "authorization" not in hops[1][1]


def test_same_host_redirect_keeps_the_token():
    hops = follow_redirect("https://api.github.com/elsewhere")
    assert hops[1][1]["authorization"] == "Bearer SECRET"


def test_default_opener_is_the_redirect_safe_one():
    assert gh_api.GitHub("T")._open is gh_api._default_open
