import sys

import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402

DIGEST = bytes(range(32))


def write_image(path, magic=b"\x32\x54\xcd\xab", digest=DIGEST):
    head = bytearray(bench.APP_DESC_ELF_SHA_OFFSET + 32 + 64)
    head[32:36] = magic
    head[bench.APP_DESC_ELF_SHA_OFFSET:bench.APP_DESC_ELF_SHA_OFFSET + 32] = digest
    path.write_bytes(bytes(head))


def test_image_digest_read_from_descriptor(tmp_path):
    img = tmp_path / "CrossPad.bin"
    write_image(img)
    assert bench.image_elf_sha256(str(img)) == DIGEST.hex()


def test_image_without_app_descriptor_has_no_digest(tmp_path):
    img = tmp_path / "CrossPad.bin"
    write_image(img, magic=b"\0\0\0\0")
    assert bench.image_elf_sha256(str(img)) is None


def test_short_or_missing_image_has_no_digest(tmp_path):
    img = tmp_path / "CrossPad.bin"
    img.write_bytes(b"\xe9" * 40)
    assert bench.image_elf_sha256(str(img)) is None
    assert bench.image_elf_sha256(str(tmp_path / "absent.bin")) is None


def test_newest_run_holding_the_artifact_wins():
    runs = [
        {"databaseId": 3, "artifacts": []},                                   # still building
        {"databaseId": 2, "artifacts": [{"name": "build-v2-full", "expired": False}]},
        {"databaseId": 1, "artifacts": [{"name": "build-v2-full", "expired": False}]},
    ]
    run, art = bench.pick_trial_run(runs, "build-v2-full")
    assert run["databaseId"] == 2 and art["name"] == "build-v2-full"


def test_expired_or_other_revision_artifacts_are_skipped():
    runs = [
        {"databaseId": 2, "artifacts": [{"name": "build-v2-full", "expired": True}]},
        {"databaseId": 1, "artifacts": [{"name": "build-v1-full", "expired": False}]},
    ]
    assert bench.pick_trial_run(runs, "build-v2-full") == (None, None)


def test_run_without_listing_is_skipped():
    assert bench.pick_trial_run([{"databaseId": 1, "artifacts": None}], "build-v2-full") == (None, None)


def test_image_arg_takes_the_dir_or_the_file_in_it(tmp_path, monkeypatch):
    (tmp_path / "CrossPad.bin").write_bytes(b"x")
    assert bench.trial_image_dir(str(tmp_path)) == str(tmp_path)
    assert bench.trial_image_dir(str(tmp_path / "CrossPad.bin")) == str(tmp_path)
    # A bare file name in the cwd: the cwd, never an empty (falsy) dir.
    monkeypatch.chdir(tmp_path)
    assert bench.trial_image_dir("CrossPad.bin") == str(tmp_path)


def test_cut_short_download_leaves_no_cached_image(tmp_path, monkeypatch):
    import subprocess
    monkeypatch.setattr(bench, "REPO", str(tmp_path))
    monkeypatch.setattr(bench.shutil, "which", lambda _: "gh")
    replies = {
        ("pr", "view"): '{"headRefOid": "abcdef0123456789", "title": "t"}',
        ("run", "list"): '[{"databaseId": 7, "status": "completed", "conclusion": "success"}]',
        ("api",): '{"artifacts": [{"name": "build-v2-full", "expired": false}]}',
    }

    def fake_run(cmd, **kw):
        a = cmd[1:]
        if a[:2] == ["run", "download"]:
            dest = a[a.index("-D") + 1]
            (Path(dest) / "CrossPad.bin").write_bytes(b"half")
            raise KeyboardInterrupt                                # Ctrl-C mid-download
        out = replies.get(tuple(a[:2])) or replies.get(tuple(a[:1]))
        return subprocess.CompletedProcess(cmd, 0, out, "")

    monkeypatch.setattr(bench.subprocess, "run", fake_run)
    with pytest.raises(KeyboardInterrupt):
        bench.fetch_pr_build(5, "v2")
    # The half file is not where the next call looks for a finished download.
    assert not (tmp_path / ".crosspad" / "trial" / "pr5-abcdef012" / "CrossPad.bin").exists()
