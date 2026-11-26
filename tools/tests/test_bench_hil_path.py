import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench as b  # noqa: E402


def norm(p):
    return p.replace("\\", "/")


def test_the_environment_override_wins_but_only_if_it_exists():
    # A typo in CROSSPAD_HIL must not send the tool off to a nonexistent binary;
    # falling through to PATH is what lets a bench with a broken override still run.
    got = b.hil_path(environ={"CROSSPAD_HIL": "/x/hil"},
                     exists=lambda p: p == "/x/hil", which=lambda n: None)
    assert got == "/x/hil"
    got = b.hil_path(environ={"CROSSPAD_HIL": "/does/not/exist"},
                     exists=lambda p: False, which=lambda n: "/y/hil")
    assert got == "/y/hil"


def test_path_wins_over_the_known_layouts():
    got = b.hil_path(environ={}, exists=lambda p: True, which=lambda n: "/on/path/hil")
    assert got == "/on/path/hil"


def test_the_first_existing_candidate_is_the_one_returned():
    hit = b.HIL_CANDIDATES[-1]
    got = b.hil_path(environ={}, exists=lambda p: p == hit, which=lambda n: None)
    assert got == hit


def test_a_crosspad_hil_checkout_beside_this_repo_is_a_candidate():
    """The Windows bench bug this fixes: a workspace of two checkouts side by side,
    with the venv in Windows' Scripts/ layout. bench.py looked in three places and
    found none of them, so `check` said "not found" and every command that touches
    the board refused to run -- including ota_flash.py, which goes through
    crosspad_board.list_devices() and reported "crosspad-hil lists no board"."""
    cands = [norm(p) for p in b.HIL_CANDIDATES]
    parent = norm(str(Path(b.REPO).parent))
    assert f"{parent}/crosspad-hil/.venv/Scripts/crosspad-hil.exe" in cands   # Windows
    assert f"{parent}/crosspad-hil/.venv/bin/crosspad-hil" in cands           # Linux/macOS
    # The repo's own .venv, both layouts, still come first: that is where the
    # CrossPad installer puts it, and the order is what makes it win.
    assert cands[0].endswith("/.venv/bin/crosspad-hil")
    assert cands[1].endswith("/.venv/Scripts/crosspad-hil.exe")


def test_no_candidate_installed_means_none_not_an_error():
    assert b.hil_path(environ={}, exists=lambda p: False, which=lambda n: None) is None
