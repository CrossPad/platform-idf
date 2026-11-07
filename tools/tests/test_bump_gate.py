import copy, json, sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bump_gate

A, B = "a" * 40, "b" * 40
PR = {"head": {"ref": "auto/app-bump", "repo": {"full_name": "CrossPad/platform-idf"}},
      "base": {"repo": {"full_name": "CrossPad/platform-idf"}}, "user": {"login": "crosspad-bot[bot]"}}
COMMITS = [{"author": {"login": "crosspad-bot[bot]"}, "commit": {"verification": {"verified": True}}}]
BASE = {"full": {"name": "full", "apps": {"mixer": {"track": "pinned", "commit": A},
                                          "song": {"track": "pinned", "commit": A}}}}
RAW = f":160000 160000 {A} {B} M\tcomponents/crosspad-mixer\n:100644 100644 1111111 2222222 M\tconfig/profiles/full.json\n"


def head_profiles():
    h = copy.deepcopy(BASE)
    h["full"]["apps"]["mixer"]["commit"] = B
    return h


def judge(**over):
    args = dict(pr=PR, commits=COMMITS, raw=bump_gate.parse_raw_diff(RAW), base_profiles=BASE,
                head_profiles=head_profiles(), official=["mixer", "song"], build_result="success")
    args.update(over)
    return bump_gate.judge(**args)


def test_non_bump_pr_passes():
    human = copy.deepcopy(PR)
    human["head"]["ref"] = "feat/x"
    human["user"]["login"] = "matixan"
    assert bump_gate.is_bump(human) is False


def test_a_clean_bump_passes():
    assert judge() == []


def test_wrong_author_fork_or_extra_commit_fail():
    pr = copy.deepcopy(PR)
    pr["user"]["login"] = "someone"
    assert any("author" in p for p in judge(pr=pr))
    fork = copy.deepcopy(PR)
    fork["head"]["repo"]["full_name"] = "evil/platform-idf"
    assert any("fork" in p for p in judge(pr=fork))
    assert any("exactly one commit" in p for p in judge(commits=COMMITS * 2))
    unverified = [{"author": {"login": "crosspad-bot[bot]"}, "commit": {"verification": {"verified": False}}}]
    assert any("verified" in p for p in judge(commits=unverified))


def test_other_paths_fail():
    raw = bump_gate.parse_raw_diff(RAW + ":100644 100644 1 2 M\tmain/main.cpp\n"
                                         ":100644 100644 1 2 M\tconfig/profiles/community/x.json\n")
    problems = judge(raw=raw)
    assert any("main/main.cpp" in p for p in problems)
    assert any("community/x.json" in p for p in problems)


def test_gitlink_of_a_non_official_path_fails():
    raw = bump_gate.parse_raw_diff(RAW + f":160000 160000 {A} {B} M\tcomponents/crosspad-foo\n")
    assert any("components/crosspad-foo" in p for p in judge(raw=raw))


def test_a_core_or_gui_gitlink_move_passes():
    raw = bump_gate.parse_raw_diff(RAW + f":160000 160000 {A} {B} M\tcomponents/crosspad-core\n"
                                         f":160000 160000 {A} {B} M\tcomponents/crosspad-gui\n")
    assert judge(raw=raw) == []


def test_a_core_move_alone_passes_with_profiles_untouched():
    raw = bump_gate.parse_raw_diff(f":160000 160000 {A} {B} M\tcomponents/crosspad-core\n")
    assert judge(raw=raw, head_profiles=copy.deepcopy(BASE)) == []


@pytest.mark.parametrize("row", [f":000000 160000 {'0' * 40} {B} A\tcomponents/crosspad-core\n",
                                 f":160000 000000 {A} {'0' * 40} D\tcomponents/crosspad-gui\n",
                                 f":160000 100644 {A} {B} T\tcomponents/crosspad-core\n"])
def test_a_library_gitlink_added_deleted_or_retyped_fails(row):
    problems = judge(raw=bump_gate.parse_raw_diff(RAW + row))
    assert any("library gitlink may only move" in p for p in problems)


def test_profile_change_beyond_the_pin_fails():
    h = head_profiles()
    h["full"]["apps"]["song"] = {"track": "branch", "ref": "main"}
    assert any("full.json" in p for p in judge(head_profiles=h))
    h2 = head_profiles()
    h2["full"]["features"] = {"X": True}
    assert any("full.json" in p for p in judge(head_profiles=h2))


def test_pin_must_equal_the_new_gitlink():
    h = head_profiles()
    h["full"]["apps"]["mixer"]["commit"] = A
    assert any("full.json" in p for p in judge(head_profiles=h))


@pytest.mark.parametrize("result", ["failure", "skipped", "cancelled", ""])
def test_a_build_that_is_not_green_fails(result):
    # skipped: the matrix job failed, so build never ran; the gate still runs (!cancelled()) and says no
    assert any("build" in p for p in judge(build_result=result))


def test_main_yml_gate_runs_after_a_failed_or_skipped_build_but_not_a_cancelled_run():
    doc = yaml.safe_load((Path(__file__).resolve().parents[2] / ".github" / "workflows" / "main.yml").read_text())
    job = doc["jobs"]["bump-gate"]
    assert job["if"] == "!cancelled() && github.event_name == 'pull_request'"
    assert job["needs"] == ["matrix", "build"]
    run = job["steps"][-1]
    assert run["env"]["BUILD_RESULT"] == "${{ needs.build.result }}"
    assert '--build-result "$BUILD_RESULT"' in run["run"]


# Beyond the brief: holes a reviewer would find.
def test_a_deleted_or_added_profile_fails():
    assert any("lite.json" in p for p in judge(base_profiles=BASE | {"lite": {"apps": {}}}))
    assert any("extra.json" in p for p in judge(head_profiles=head_profiles() | {"extra": {"apps": {}}}))


def test_a_pin_without_a_moved_gitlink_fails():
    raw = bump_gate.parse_raw_diff(RAW.splitlines()[1] + "\n")
    assert any("full.json" in p for p in judge(raw=raw))


def test_added_deleted_or_retyped_gitlink_fails():
    for line in (f":000000 160000 {'0' * 40} {B} A\tcomponents/crosspad-mixer",
                 f":160000 000000 {A} {'0' * 40} D\tcomponents/crosspad-mixer",
                 f":160000 100644 {A} {B} T\tcomponents/crosspad-mixer"):
        assert any("components/crosspad-mixer" in p for p in judge(raw=bump_gate.parse_raw_diff(line + "\n")))


def test_a_profile_that_is_not_a_regular_file_fails():
    raw = bump_gate.parse_raw_diff(RAW.replace(":100644 100644", ":100644 120000"))
    assert any("full.json" in p for p in judge(raw=raw))


def test_head_repo_gone_counts_as_a_fork():
    pr = copy.deepcopy(PR)
    pr["head"]["repo"] = None
    assert any("fork" in p for p in judge(pr=pr))


def test_commit_without_a_matching_account_fails():
    ghost = [{"author": None, "commit": {"verification": {"verified": True}}}]
    assert any("authored" in p for p in judge(commits=ghost))


def test_parse_raw_diff_fields_and_noise():
    assert bump_gate.parse_raw_diff(RAW) == [("160000", "160000", B, "components/crosspad-mixer"),
                                             ("100644", "100644", "2222222", "config/profiles/full.json")]
    assert bump_gate.parse_raw_diff("warning: something\n") == []


# main(): the facts it cannot determine.
class FakeGit:
    """Stands in for subprocess.run of git: maps argv prefixes to stdout."""
    def __init__(self, out): self.out, self.calls = out, []

    def __call__(self, cmd, **kw):
        self.calls.append(cmd)
        for key, text in self.out.items():
            if key in cmd:
                if isinstance(text, Exception):
                    raise text
                return type("R", (), {"stdout": text})()
        raise AssertionError(f"unexpected git call {cmd}")


def event(tmp_path, pr):
    p = tmp_path / "event.json"
    p.write_text(json.dumps({"pull_request": pr}))
    return str(p)


C, H = "c" * 40, "d" * 40
FULL_PR = PR | {"number": 7, "base": PR["base"] | {"sha": C}, "head": PR["head"] | {"sha": H}}


def run_main(tmp_path, monkeypatch, pr, **over):
    class Gh:
        def paginate(self, path):
            assert path == "/repos/CrossPad/platform-idf/pulls/7/commits"
            yield from over.get("commits", [dict(COMMITS[0], sha=H)])
    monkeypatch.setattr(bump_gate.gh_api, "from_env", over.get("from_env", lambda: Gh()))
    monkeypatch.setattr(bump_gate.app_bump, "official_apps", lambda root: ["mixer", "song"])
    profiles = over.get("profiles", {C: BASE, H: head_profiles()})
    monkeypatch.setattr(bump_gate, "_profiles_at", lambda ref: profiles[ref])
    monkeypatch.setattr(bump_gate.subprocess, "run", over.get("git", FakeGit({"diff": RAW, "cat-file": ""})))
    return bump_gate.main(["--event", event(tmp_path, pr), "--build-result", over.get("build", "success")])


def test_main_passes_a_clean_bump(tmp_path, monkeypatch, capsys):
    assert run_main(tmp_path, monkeypatch, FULL_PR) == 0
    assert "pass" in capsys.readouterr().out


def test_main_non_bump_touches_nothing(tmp_path, monkeypatch, capsys):
    human = copy.deepcopy(FULL_PR)
    human["head"]["ref"] = "feat/x"

    def boom(*a, **k):
        raise AssertionError("must not be called")
    assert run_main(tmp_path, monkeypatch, human, from_env=boom, git=boom, profiles={}, build="failure") == 0
    assert "not a bump PR" in capsys.readouterr().out


@pytest.mark.parametrize("what", ["api", "git", "profiles", "head_gone"])
def test_main_bump_with_undeterminable_facts_fails(tmp_path, monkeypatch, capsys, what):
    def api_down():
        raise SystemExit("GH_TOKEN is not set")
    over = {"api": {"from_env": api_down},
            "git": {"git": FakeGit({"diff": bump_gate.subprocess.CalledProcessError(128, "git"), "cat-file": ""})},
            "head_gone": {"git": FakeGit({"cat-file": bump_gate.subprocess.CalledProcessError(1, "git")})},
            "profiles": {"profiles": {}}}[what]
    assert run_main(tmp_path, monkeypatch, FULL_PR, **over) == 1
    assert "::error::" in capsys.readouterr().out


def main_on(tmp_path, text):
    p = tmp_path / "e.json"
    p.write_text(text)
    return bump_gate.main(["--event", str(p), "--build-result", "success"])


@pytest.mark.parametrize("payload", [{}, {"pull_request": None}])
def test_main_an_event_without_a_pr_fails_closed(tmp_path, capsys, payload):
    assert main_on(tmp_path, json.dumps(payload)) == 1
    assert "::error::" in capsys.readouterr().out


@pytest.mark.parametrize("text", ["not json", json.dumps({"pull_request": {"head": None}})])
def test_main_an_event_it_cannot_read_fails_closed(tmp_path, capsys, text):
    # Whether the PR is the bot's is the one fact that decides; without it, no pass.
    assert main_on(tmp_path, text) == 1
    assert "::error::" in capsys.readouterr().out


def test_error_text_cannot_carry_a_newline(tmp_path, monkeypatch, capsys):
    def api_down():
        raise RuntimeError("down\n::add-mask::x")
    assert run_main(tmp_path, monkeypatch, FULL_PR, from_env=api_down) == 1
    out = capsys.readouterr().out
    assert "::error::" in out
    assert all(line.startswith(("::error::", "bump-gate:")) for line in out.splitlines())


def test_real_git_profiles_and_diff(tmp_path, monkeypatch):
    import os, subprocess
    env = os.environ | {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                        "GIT_COMMITTER_EMAIL": "t@t"}

    def git(*a):
        return subprocess.run(["git", *a], cwd=tmp_path, env=env, check=True, capture_output=True, text=True).stdout
    git("init", "-q", "-b", "main")
    prof = tmp_path / "config" / "profiles"
    (prof / "community").mkdir(parents=True)
    (prof / "full.json").write_text(json.dumps(BASE["full"]))
    (prof / "community" / "x.json").write_text("{}")
    (prof / "odd name.json").write_text("{}")
    (prof / "link.json").symlink_to("full.json")
    git("add", "-A"); git("commit", "-qm", "base")
    base = git("rev-parse", "HEAD").strip()
    (prof / "full.json").write_text(json.dumps(head_profiles()["full"]))
    git("commit", "-qam", "bump")
    monkeypatch.chdir(tmp_path)
    assert set(bump_gate._profiles_at(base)) == {"full", "odd name"}
    assert bump_gate._profiles_at("HEAD")["full"]["apps"]["mixer"]["commit"] == B
    rows = bump_gate.parse_raw_diff(bump_gate._git("diff", "--raw", "--no-abbrev", "--no-renames", f"{base}...HEAD", "--"))
    assert [r[3] for r in rows] == ["config/profiles/full.json"]


def test_commit_that_is_not_the_event_head_fails():
    stale = [dict(COMMITS[0], sha="e" * 40)]
    assert any("head" in p for p in judge(commits=stale, head_sha=H))
    assert judge(commits=[dict(COMMITS[0], sha=H)], head_sha=H) == []


def test_main_api_commit_differs_from_event_head(tmp_path, monkeypatch, capsys):
    moved = [dict(COMMITS[0], sha="e" * 40)]  # the PR got a new commit after this event
    assert run_main(tmp_path, monkeypatch, FULL_PR, commits=moved) == 1
    assert "newest commit is not the head" in capsys.readouterr().out


@pytest.mark.parametrize("sha", [None, "abc", "D" * 40, "-" + "d" * 39])
def test_main_head_sha_must_be_full_hex(tmp_path, monkeypatch, capsys, sha):
    pr = copy.deepcopy(FULL_PR)
    pr["head"]["sha"] = sha
    assert run_main(tmp_path, monkeypatch, pr) == 1
    assert "not a full commit id" in capsys.readouterr().out


def test_main_judges_at_the_event_sha_not_a_ref(tmp_path, monkeypatch):
    git = FakeGit({"diff": RAW, "cat-file": ""})
    assert run_main(tmp_path, monkeypatch, FULL_PR, git=git) == 0
    assert ["git", "cat-file", "-e", f"{H}^{{commit}}"] in git.calls
    assert any(f"{C}...{H}" in c for c in git.calls)


def test_main_yml_tag_alert_runs_after_a_failed_tag_build():
    doc = yaml.safe_load((Path(__file__).resolve().parents[2] / ".github" / "workflows" / "main.yml").read_text())
    job, release = doc["jobs"]["tag-alert"], doc["jobs"]["release"]
    assert job["needs"] == ["matrix", "build"]
    assert job["if"] == "failure() && startsWith(github.ref, 'refs/tags/v')"
    assert job["permissions"] == {"contents": "read"}
    assert "environment" not in job
    assert job["runs-on"] == release["runs-on"] and job["container"] == release["container"]
    assert any(s.get("uses", "").startswith("actions/checkout@") for s in job["steps"])
    runs = [s["run"] for s in job["steps"] if "run" in s]
    assert runs and not any("${{" in r for r in runs)
    alert = next(s for s in job["steps"] if s.get("name") == "Alert")
    assert alert["env"] == {"DISCORD_CI_ALERTS_WEBHOOK": "${{ secrets.DISCORD_CI_ALERTS_WEBHOOK }}"}
    assert 'tools/ci_alert.py "tag build red: $GITHUB_REF_NAME"' in alert["run"]
