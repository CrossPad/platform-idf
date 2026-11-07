#!/usr/bin/env python3
"""Run this repository's GitHub Actions on this machine -- no Actions minutes,
and ccache/idf caches stay warm between runs.

    python tools/ci_runner.py install   # download + register a runner (~/actions-runner)
    python tools/ci_runner.py service   # systemd --user unit, starts on login (linger on)
    python tools/ci_runner.py on        # CI_RUNNER=self-hosted: jobs come here
    python tools/ci_runner.py off       # back to ubuntu-latest (do this before shutting down!)
    python tools/ci_runner.py status    # runner + variable state

The workflow reads ``vars.CI_RUNNER`` (.github/workflows/main.yml); with it
unset nothing changes. GitHub never falls back from ``self-hosted`` to a hosted
runner, so a job queued against a powered-off runner waits until ``off``.
Container jobs (crosspad-ci, tools/ci/) need docker usable without sudo. Registration
is per repository -- the token endpoint works with the ``repo`` scope; an
org-wide runner would need admin:org.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO = os.environ.get("CI_RUNNER_REPO", "CrossPad/platform-idf")
DIR = Path(os.environ.get("CI_RUNNER_DIR", Path.home() / "actions-runner"))
NAME = os.environ.get("CI_RUNNER_NAME", f"{socket.gethostname()}-runner")
LABELS = os.environ.get("CI_RUNNER_LABELS", "self-hosted,linux,x64")
UNIT = "github-runner-" + REPO.replace("/", "-")
ASSET = re.compile(r"actions-runner-linux-x64-[0-9.]+\.tar\.gz$")


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def run(*argv: str, capture: bool = False, check: bool = True, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(argv, text=True, capture_output=capture, check=check, **kw)


def gh_json(*args: str):
    return json.loads(run("gh", "api", *args, capture=True).stdout)


def cmd_install() -> None:
    for tool in ("gh", "docker"):
        if not shutil.which(tool):
            die(f"{tool} missing")
    if run("docker", "ps", capture=True, check=False).returncode != 0:
        die("docker not usable without sudo (add yourself to the docker group)")
    DIR.mkdir(parents=True, exist_ok=True)
    if not (DIR / "config.sh").exists():
        assets = gh_json("repos/actions/runner/releases/latest")["assets"]
        url = next(a["browser_download_url"] for a in assets if ASSET.search(a["name"]))
        print(f"downloading {url}")
        tarball = DIR / "runner.tar.gz"
        urllib.request.urlretrieve(url, tarball)
        with tarfile.open(tarball) as tf:
            tf.extractall(DIR)
        tarball.unlink()
    if (DIR / ".runner").exists():
        agent = json.loads((DIR / ".runner").read_text(encoding="utf-8-sig"))["agentName"]
        print(f"already registered: {agent}")
        return
    # Short-lived (1 h) token; it goes straight to config.sh, never to disk.
    token = gh_json("-X", "POST", f"repos/{REPO}/actions/runners/registration-token")["token"]
    run(str(DIR / "config.sh"), "--unattended", "--url", f"https://github.com/{REPO}",
        "--token", token, "--name", NAME, "--labels", LABELS, "--work", "_work", "--replace",
        cwd=DIR)
    print(f"registered {NAME} for {REPO}")


def cmd_service() -> None:
    if not (DIR / "run.sh").exists():
        die(f"no runner in {DIR} -- run install first")
    unit_dir = Path.home() / ".config/systemd/user"
    unit_dir.mkdir(parents=True, exist_ok=True)
    (unit_dir / f"{UNIT}.service").write_text(f"""[Unit]
Description=GitHub Actions runner ({REPO})
After=network-online.target

[Service]
WorkingDirectory={DIR}
ExecStart={DIR}/run.sh
Restart=always
RestartSec=10
KillMode=process
KillSignal=SIGINT
TimeoutStopSec=5min

[Install]
WantedBy=default.target
""")
    run("systemctl", "--user", "daemon-reload")
    run("systemctl", "--user", "enable", "--now", f"{UNIT}.service")
    linger = run("loginctl", "show-user", os.environ.get("USER", ""), "-p", "Linger",
                 capture=True, check=False).stdout
    if "yes" not in linger:
        print("note: enable lingering (loginctl enable-linger $USER) so the runner survives logout")
    run("systemctl", "--user", "--no-pager", "status", f"{UNIT}.service", check=False)


def cmd_on() -> None:
    run("gh", "variable", "set", "CI_RUNNER", "--body", "self-hosted", "-R", REPO)
    print(f"CI_RUNNER=self-hosted on {REPO}")


def cmd_off() -> None:
    run("gh", "variable", "delete", "CI_RUNNER", "-R", REPO, check=False, capture=True)
    print(f"CI_RUNNER unset on {REPO} (ubuntu-latest)")


def cmd_status() -> None:
    print(f"repo:     {REPO}")
    listing = run("gh", "variable", "list", "-R", REPO, capture=True, check=False).stdout
    value = next((line.split()[1] for line in listing.splitlines()
                  if line.split() and line.split()[0] == "CI_RUNNER"), "(unset)")
    print(f"variable: CI_RUNNER={value}")
    print("runners:")
    for r in gh_json(f"repos/{REPO}/actions/runners").get("runners", []):
        labels = ",".join(lb["name"] for lb in r["labels"])
        print(f"  {r['name']}  {r['status']}  busy={r['busy']}  labels={labels}")
    active = run("systemctl", "--user", "is-active", f"{UNIT}.service",
                 capture=True, check=False).stdout.strip()
    print(f"service:  {active}")


def main() -> None:
    cmds = {"install": cmd_install, "service": cmd_service, "on": cmd_on,
            "off": cmd_off, "status": cmd_status}
    if len(sys.argv) != 2 or sys.argv[1] not in cmds:
        print(__doc__.strip())
        sys.exit(1)
    cmds[sys.argv[1]]()


if __name__ == "__main__":
    main()
