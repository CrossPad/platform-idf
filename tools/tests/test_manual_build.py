import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "docs" / "manual"))
import build as mb  # noqa: E402


def spec(tmp, name="en"):
    return {"dir": tmp / "chapters" / name, "toc_label": "Contents", "html_lang": "en",
            "title": "t", "html": "x.full.html", "pdf": "x.pdf"}


def test_toc_numbers_and_labels_the_chapters_that_are_there():
    chapters = ['<h1 id="a">Alpha</h1>', '<h1 id="b">Beta &amp; Bitter</h1>']
    out = mb.toc(chapters, "Contents")
    assert "<h1>Contents</h1>" in out                 # the label comes from the language
    assert '1. <a href="#a">Alpha' in out and '2. <a href="#b">Beta' in out
    assert "Spis" not in out


def test_polish_defaults_are_what_they_were():
    pl = mb.LANGS["pl"]
    assert pl["dir"] == mb.CHAPTERS
    assert pl["toc_label"] == "Spis treści"
    assert pl["html"] == "crosspad-instrukcja.full.html"
    assert pl["pdf"] == "CrossPad-instrukcja.pdf"
    assert mb.LANGS["en"]["dir"] == mb.CHAPTERS / "en"
    assert mb.LANGS["en"]["pdf"] == "CrossPad-instructions.pdf"


def test_assemble_rewrites_image_paths_at_the_depth_of_the_language(tmp_path, monkeypatch):
    (tmp_path / "style.css").write_text("body{}", encoding="utf-8")
    deep = tmp_path / "chapters" / "en"
    deep.mkdir(parents=True)
    (deep / "02-x.html").write_text('<h1 id="x">X</h1><img src="img/home.png">', encoding="utf-8")
    monkeypatch.setattr(mb, "ROOT", tmp_path)
    doc, count = mb.assemble(spec(tmp_path))
    assert count == 1
    assert '../../img/home.png' in doc          # chapters/en is one level deeper
    assert '<html lang="en">' in doc


def test_a_link_into_an_untranslated_chapter_fails_the_language(tmp_path, monkeypatch):
    # This is what keeps a partial edition honest: an href to a chapter that is
    # not there yet is an error, not a page that prints half-broken.
    (tmp_path / "style.css").write_text("body{}", encoding="utf-8")
    d = tmp_path / "chapters" / "en"
    d.mkdir(parents=True)
    (d / "02-x.html").write_text('<h1 id="x">X</h1><a href="#obsluga">Handling</a>', encoding="utf-8")
    monkeypatch.setattr(mb, "ROOT", tmp_path)
    try:
        mb.assemble(spec(tmp_path))
    except SystemExit as e:
        assert "obsluga" in str(e)
    else:
        raise AssertionError("a broken link should stop the build")


def test_the_translated_english_example_builds():
    # The example shipped in this repo must actually assemble: three chapters,
    # and no link pointing at a chapter that does not exist yet.
    doc, count = mb.assemble(mb.LANGS["en"])
    assert count == 3
    assert "Welcome to CrossPad" in doc


def test_the_browser_is_found_on_the_machine_it_runs_on():
    calls = {}

    def fake_which(name):
        return {"msedge": r"C:\x\msedge.exe"}.get(name)

    def fake_exists(path):
        return False

    found = mb.find_chrome(None, platform="win32", exist=fake_exists, which=fake_which)
    assert found == r"C:\x\msedge.exe"          # Edge is on every Windows box

    assert mb.find_chrome("/opt/chrome", platform="win32") == "/opt/chrome"   # --chrome wins

    calls["env"] = None
    assert mb.find_chrome(None, platform="linux", exist=lambda p: False,
                          which=lambda n: None) is None                       # nothing found


def test_the_polish_original_still_builds_exactly_as_it_did():
    # CI builds this language, so the refactor has to leave it alone. Beyond the
    # unit expectations below, the branch was checked by running the old and the
    # new build.py over the same tree and comparing the generated page byte for
    # byte: identical (109 452 bytes both).
    doc, count = mb.assemble(mb.LANGS["pl"])
    assert count == 25                                  # cover + contents + 23 chapters
    assert '<html lang="pl">' in doc
    assert "<h1>Spis treści</h1>" in doc                # the generated contents label
    assert "crosspad-instrukcja" not in doc             # names live in build.py, not the page
    assert doc.endswith("</html>\n")


def test_the_env_var_wins_over_the_search(monkeypatch):
    monkeypatch.setenv("CROSSPAD_CHROME", "/somewhere/chrome")
    assert mb.find_chrome(None, platform="linux", which=lambda n: None,
                          exist=lambda p: False) == "/somewhere/chrome"
