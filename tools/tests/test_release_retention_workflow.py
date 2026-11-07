"""The retention workflow's shell step, run against a stub tool: --apply only
when apply is asked for, RETENTION is on and the ref is main."""
import subprocess
import textwrap
from pathlib import Path

import pytest

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release-retention.yml"


def run_step(tmp_path, **env):
    text = WORKFLOW.read_text()
    script = textwrap.dedent(text.split("        run: |\n", 1)[1])
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "release_retention.py").write_text("import sys; print('ARGS', *sys.argv[1:])\n")
    base = {"PATH": "/usr/bin:/bin", "GITHUB_REPOSITORY": "o/r", "MAX": "10", "APPLY": "true",
            "RETENTION": "on", "REF": "refs/heads/main"}
    r = subprocess.run(["bash", "-c", script], cwd=tmp_path, env=base | env, capture_output=True, text=True, check=True)
    return r.stdout


def test_applies_on_main_with_retention_on(tmp_path):
    assert "--apply" in run_step(tmp_path)


@pytest.mark.parametrize("env", [{"REF": "refs/heads/feat/x"}, {"REF": "refs/tags/v1.0.0"},
                                 {"RETENTION": ""}, {"RETENTION": "dry-run"}, {"APPLY": "false"}])
def test_plan_only_otherwise(tmp_path, env):
    assert "--apply" not in run_step(tmp_path, **env)


def test_notice_names_the_ref_when_skipped_for_it(tmp_path):
    out = run_step(tmp_path, REF="refs/heads/feat/x")
    assert "::notice::" in out and "refs/heads/feat/x" in out


def test_ref_is_passed_by_env_not_interpolated():
    text = WORKFLOW.read_text()
    assert "REF: ${{ github.ref }}" in text
    assert "github.ref }}" not in text.split("        run: |\n", 1)[1]
