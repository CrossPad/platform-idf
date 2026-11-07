#!/usr/bin/env python3
"""GitHub REST and GraphQL for CI tools, standard library only.

The CI jobs run in a container with git and python3, no gh.
"""
import json
import os
import urllib.error
import urllib.parse
import urllib.request

API = os.environ.get("GITHUB_API_URL", "https://api.github.com")


class ApiError(Exception):
    def __init__(self, status: int, method: str, path: str, body: str):
        super().__init__(f"{method} {path}: HTTP {status}: {body[:300]}")
        self.status = status


def _next_link(link: str) -> str | None:
    for part in link.split(","):
        seg = part.split(";")
        if len(seg) > 1 and seg[1].strip() == 'rel="next"':
            return seg[0].strip()[1:-1]
    return None


def _origin(url: str) -> tuple[str, str]:
    parts = urllib.parse.urlsplit(url)
    return parts.scheme, parts.netloc


class _DropAuthOnForeignRedirect(urllib.request.HTTPRedirectHandler):
    """urllib copies Authorization to the redirect target; an asset download
    redirects to object storage, which must never see the token."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and _origin(new.full_url) != _origin(req.full_url):
            for name in [k for k in new.headers if k.lower() == "authorization"]:
                del new.headers[name]
        return new


_default_open = urllib.request.build_opener(_DropAuthOnForeignRedirect).open


class GitHub:
    def __init__(self, token: str, api: str = API, opener=_default_open):
        self.token, self.api, self._open = token, api.rstrip("/"), opener

    def _url(self, path: str) -> str:
        """An API path, or an absolute URL (a Link: next) under the API root. The
        token goes nowhere else."""
        if path.startswith("/"):
            return f"{self.api}{path}"
        if path.startswith(self.api + "/"):
            return path
        raise ValueError(f"refusing to send credentials outside {self.api}")

    def request(self, method: str, path: str, body=None):
        url = self._url(path)
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(url, data=data, method=method, headers={
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/json",
            "User-Agent": "crosspad-ci"})
        try:
            with self._open(req, timeout=30) as r:
                raw = r.read()
                return (json.loads(raw) if raw else None), r.headers
        except urllib.error.HTTPError as e:
            raise ApiError(e.code, method, path, e.read().decode(errors="replace")) from e

    def get(self, path):
        return self.request("GET", path)[0]

    def post(self, path, body):
        return self.request("POST", path, body)[0]

    def patch(self, path, body):
        return self.request("PATCH", path, body)[0]

    def delete(self, path):
        return self.request("DELETE", path)[0]

    def paginate(self, path):
        url = f"{self.api}{path}{'&' if '?' in path else '?'}per_page=100"
        while url:
            page, headers = self.request("GET", url)
            yield from page
            url = _next_link(headers.get("Link", "") if headers else "")

    def graphql(self, query: str, **variables) -> dict:
        out = self.post("/graphql", {"query": query, "variables": variables})
        if out.get("errors"):
            raise ApiError(200, "POST", "/graphql", json.dumps(out["errors"]))
        return out["data"]


def from_env(var: str = "GH_TOKEN") -> GitHub:
    token = os.environ.get(var)
    if not token:
        raise SystemExit(f"{var} is not set")
    return GitHub(token)
