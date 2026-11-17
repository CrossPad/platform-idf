import sys
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
