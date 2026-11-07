#!/usr/bin/env python3
"""A CI alert: Discord #ci-alerts (secret DISCORD_CI_ALERTS_WEBHOOK) plus the job summary.

Never fails a job: no webhook or a failed post prints and returns. The public
release channel is the announcer's; this one is private.

  ci_alert.py <title> <body>
"""
import json
import os
import sys
import urllib.error
import urllib.request


def run_url(env) -> str:
    s, r, i = env.get("GITHUB_SERVER_URL"), env.get("GITHUB_REPOSITORY"), env.get("GITHUB_RUN_ID")
    return f"{s}/{r}/actions/runs/{i}" if s and r and i else ""


def message(title: str, body: str, url: str) -> dict:
    # Discord caps content at 2000: cut the body, never the run URL.
    tail = f"\n{url}" if url else ""
    text = f"**{title}**\n{body}"[:max(0, 1900 - len(tail))] + tail
    return {"content": text, "allowed_mentions": {"parse": []}}


def _write_summary(env, title: str, body: str) -> None:
    path = env.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"\n### CI alert: {title}\n\n{body}\n")
    except OSError as e:
        print(f"ci-alert: summary not written: {type(e).__name__}")


def send(title: str, body: str, *, env=os.environ, opener=urllib.request.urlopen) -> bool:
    print(f"ci-alert: {title}: {body}")
    _write_summary(env, title, body)
    hook = env.get("DISCORD_CI_ALERTS_WEBHOOK")
    if not hook:
        print("ci-alert: DISCORD_CI_ALERTS_WEBHOOK not set; not posted")
        return False
    # The webhook URL is the credential, and urllib and http.client put it in
    # their exception messages (ValueError, InvalidURL): log the type only.
    try:
        req = urllib.request.Request(hook, data=json.dumps(message(title, body, run_url(env))).encode(),
                                     method="POST", headers={"Content-Type": "application/json",
                                                             "User-Agent": "crosspad-ci"})
        with opener(req, timeout=15):
            return True
    except Exception as e:
        status = f" {e.code}" if isinstance(e, urllib.error.HTTPError) else ""
        print(f"ci-alert: post failed: {type(e).__name__}{status}")
        return False


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: ci_alert.py <title> <body>")
        return 0
    try:
        send(argv[0], argv[1])
    except Exception as e:
        print(f"ci-alert: failed: {type(e).__name__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
