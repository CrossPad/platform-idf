import http.client, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_alert


class Ok:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def test_without_webhook_prints_and_returns_false(tmp_path, capsys):
    summary = tmp_path / "summary.md"
    env = {"GITHUB_STEP_SUMMARY": str(summary)}
    assert ci_alert.send("t", "b", env=env) is False
    assert "DISCORD_CI_ALERTS_WEBHOOK not set" in capsys.readouterr().out
    assert "CI alert: t" in summary.read_text()


def test_with_webhook_posts_without_mentions():
    sent = []

    def op(req, timeout=None):
        sent.append(json.loads(req.data))
        return Ok()
    env = {"DISCORD_CI_ALERTS_WEBHOOK": "https://discord.test/hook", "GITHUB_SERVER_URL": "https://github.com",
           "GITHUB_REPOSITORY": "CrossPad/platform-idf", "GITHUB_RUN_ID": "9"}
    assert ci_alert.send("bump PR red", "build (v2) failed", env=env, opener=op) is True
    assert sent[0]["allowed_mentions"] == {"parse": []}
    assert "**bump PR red**" in sent[0]["content"]
    assert sent[0]["content"].endswith("https://github.com/CrossPad/platform-idf/actions/runs/9")


def test_post_failure_is_not_fatal():
    def op(req, timeout=None):
        raise OSError("down")
    assert ci_alert.send("t", "b", env={"DISCORD_CI_ALERTS_WEBHOOK": "https://x"}, opener=op) is False


def test_cli_always_exits_zero(monkeypatch):
    # no webhook and no step summary, so the CLI path can neither post nor write
    monkeypatch.delenv("DISCORD_CI_ALERTS_WEBHOOK", raising=False)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert ci_alert.main(["title", "body"]) == 0
    assert ci_alert.main([]) == 0


WEBHOOK = "123456/s3cr3t-token"


def no_network(req, timeout=None):
    raise AssertionError("a request that does not parse must not reach the opener")


def test_webhook_without_a_scheme_returns_false_and_does_not_leak(capsys):
    assert ci_alert.send("t", "b", env={"DISCORD_CI_ALERTS_WEBHOOK": WEBHOOK}, opener=no_network) is False
    out = capsys.readouterr()
    assert WEBHOOK not in out.out + out.err
    assert "ValueError" in out.out


def test_webhook_with_a_control_character_returns_false_and_does_not_leak(capsys):
    hook = "https://discord.test/api/webhooks/1/s3cr3t\x01token"

    def op(req, timeout=None):
        # what http.client does: the message carries the whole URL
        raise http.client.InvalidURL(f"URL can't contain control characters. {req.full_url!r}")
    assert ci_alert.send("t", "b", env={"DISCORD_CI_ALERTS_WEBHOOK": hook}, opener=op) is False
    out = capsys.readouterr()
    assert "s3cr3t" not in out.out + out.err
    assert "InvalidURL" in out.out


def test_cli_with_a_bad_webhook_exits_zero_and_does_not_leak(monkeypatch, capsys):
    monkeypatch.setenv("DISCORD_CI_ALERTS_WEBHOOK", WEBHOOK)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    assert ci_alert.main(["title", "body"]) == 0
    out = capsys.readouterr()
    assert WEBHOOK not in out.out + out.err


def test_main_exits_zero_even_if_send_raises(monkeypatch):
    def boom(*a, **k):
        raise RuntimeError("x")
    monkeypatch.setattr(ci_alert, "send", boom)
    assert ci_alert.main(["t", "b"]) == 0


def test_unwritable_summary_still_posts(tmp_path, capsys):
    sent = []

    def op(req, timeout=None):
        sent.append(req.full_url)
        return Ok()
    env = {"GITHUB_STEP_SUMMARY": str(tmp_path / "missing-dir" / "summary.md"),
           "DISCORD_CI_ALERTS_WEBHOOK": "https://discord.test/hook"}
    assert ci_alert.send("t", "b", env=env, opener=op) is True
    assert sent == ["https://discord.test/hook"]
    assert "summary not written" in capsys.readouterr().out


def test_long_body_is_cut_before_the_run_url_not_after():
    url = "https://github.com/CrossPad/platform-idf/actions/runs/9"
    msg = ci_alert.message("title", "x" * 3000, url)
    assert msg["content"].endswith(url)
    assert len(msg["content"]) <= 1900
    assert msg["content"].startswith("**title**\n")
