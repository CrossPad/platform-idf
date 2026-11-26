#!/usr/bin/env python3
"""The CrossPad user manual: chapters/*.html -> build/*.full.html -> PDF.

Every file in the language's chapter directory is one <section>, taken in name
order. 00-cover is the cover; 01-toc is rebuilt here from the <h1 id> of every
chapter after it, so a chapter added or renamed lands in the contents by itself.
style.css is inlined. The PDF is printed by headless Chrome (CSS paged media:
A4, page numbers in the footer, a chapter a page).

Two languages now, one directory each:

    pl -- the Polish original, `chapters/*.html`            -> CrossPad-instrukcja.pdf
    en -- the English edition, `chapters/en/*.html`         -> CrossPad-instructions.pdf

`pl` is the default and its file names, chapter set and output are exactly what
they were, so nothing built before changes. `en` is a start, not an edition: the
cover, the contents and one chapter are translated (see README.md here for the
checklist of what is left and the rules a translation follows), and the build
says out loud how many chapters a language has so nobody mistakes a three-page
sample for the manual.

usage: python3 docs/manual/build.py [--lang pl|en] [--html-only] [--chrome PATH]

`--html-only` stops at the page and is what to run when there is no browser to
hand. The browser used to be a hard-coded macOS path; it is now found per
platform (`--chrome`, then $CROSSPAD_CHROME, then the usual names, including
msedge.exe, which exists on every Windows box and prints a PDF the same way),
and a missing one is a message with the command to re-run, not a traceback.
"""
import argparse
import html
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
CHAPTERS = ROOT / "chapters"
BUILD = ROOT / "build"

LANGS = {
    "pl": {
        "dir": CHAPTERS,
        "toc_label": "Spis treści",
        "html_lang": "pl",
        "title": "CrossPad — instrukcja obsługi",
        "html": "crosspad-instrukcja.full.html",
        "pdf": "CrossPad-instrukcja.pdf",
    },
    "en": {
        "dir": CHAPTERS / "en",
        "toc_label": "Contents",
        "html_lang": "en",
        "title": "CrossPad — User Manual",
        "html": "crosspad-instructions.full.html",
        "pdf": "CrossPad-instructions.pdf",
    },
}

# Chrome/Chromium/Edge, in the order a person on that platform is likely to have.
CHROME_CANDIDATES = {
    "darwin": ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
               "/Applications/Chromium.app/Contents/MacOS/Chromium",
               "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"],
    "linux": ["google-chrome", "google-chrome-stable", "chromium", "chromium-browser",
              "microsoft-edge", "microsoft-edge-stable"],
    "windows": ["msedge", "msedge.exe", "chrome", "chrome.exe"],
}
# Where Edge and Chrome install themselves on Windows, when they are not on PATH.
WINDOWS_PROGRAM_DIRS = ("Microsoft/Edge/Application/msedge.exe",
                        "Google/Chrome/Application/chrome.exe")


def find_chrome(explicit=None, platform=None, exist=os.path.exists, which=shutil.which):
    """The browser to print the PDF with, or None.

    Order: --chrome, then $CROSSPAD_CHROME, then the platform's usual names
    (on PATH first, then the install directories), so the build works on the
    machine it happens to be run on instead of on one laptop.
    """
    if explicit:
        return explicit
    env = os.environ.get("CROSSPAD_CHROME")
    if env:
        return env
    sysname = (platform or sys.platform or "").lower()
    if "darwin" in sysname:
        names = CHROME_CANDIDATES["darwin"]
    elif "win" in sysname:
        names = CHROME_CANDIDATES["windows"]
    else:
        names = CHROME_CANDIDATES["linux"]
    for name in names:
        found = which(name)
        if found:
            return found
    local = os.environ.get("PROGRAMFILES", r"C:\Program Files")
    for rel in WINDOWS_PROGRAM_DIRS:
        candidate = os.path.join(local, rel)
        if exist(candidate):
            return candidate
    return None


def toc(chapters, label):
    """The contents page, rebuilt from what is actually here."""
    rows = []
    n = 0
    for text in chapters:
        m = re.search(r'<h1 id="([^"]+)">(.*?)</h1>', text, re.S)
        if not m:
            continue
        n += 1
        title = re.sub(r"<[^>]+>", "", m.group(2)).strip()
        rows.append(f'<div>{n}. <a href="#{m.group(1)}">{html.escape(title, quote=False)}</a></div>')
    return (f'<section class="chapter">\n<h1>{label}</h1>\n<div class="toc">\n'
            + "\n".join(rows) + "\n</div>\n</section>\n")


def assemble(spec):
    """One language's page, as text. Returns (document, chapter_count)."""
    chapter_dir = spec["dir"]
    if not chapter_dir.is_dir():
        sys.exit(f"no chapters for this language: {chapter_dir}")
    files = sorted(chapter_dir.glob("*.html"))
    if not files:
        sys.exit(f"no chapters for this language in {chapter_dir}")
    texts = [f.read_text(encoding="utf-8") for f in files]
    body = []
    for f, t in zip(files, texts):
        if f.name.startswith("01-toc"):
            body.append(toc([x for g, x in zip(files, texts) if g.name > f.name],
                            spec["toc_label"]))
        else:
            body.append(t)
    # every internal link must point at an id that exists
    ids = set(re.findall(r'id="([^"]+)"', "".join(body)))
    missing = sorted(set(re.findall(r'href="#([^"]+)"', "".join(body))) - ids)
    if missing:
        sys.exit("broken links: " + ", ".join(missing))
    # chapters say img/x.png; the page is printed from build/, and English
    # chapters live one level deeper -- both reach the same images.
    depth = len(chapter_dir.relative_to(ROOT).parts)
    up = "../" * depth
    body = [b.replace('src="img/', f'src="{up}img/') for b in body]
    style = (ROOT / "style.css").read_text(encoding="utf-8")
    doc = (f'<!doctype html>\n<html lang="{spec["html_lang"]}">\n<head>\n<meta charset="utf-8">\n'
           f'<title>{spec["title"]}</title>\n<style>\n' + style + "</style>\n</head>\n<body>\n\n"
           + "\n".join(body) + "\n</body>\n</html>\n")
    return doc, len(files)


def main(argv=None):
    ap = argparse.ArgumentParser(description="build the CrossPad manual PDF")
    ap.add_argument("--lang", default="pl", choices=sorted(LANGS),
                    help="which language to build (default: pl, the Polish original)")
    ap.add_argument("--html-only", action="store_true", help="stop at the page, print no PDF")
    ap.add_argument("--chrome", default=None, help="browser to print with")
    args = ap.parse_args(argv)

    spec = LANGS[args.lang]
    doc, count = assemble(spec)
    BUILD.mkdir(exist_ok=True)
    out_html = BUILD / spec["html"]
    out_html.write_text(doc, encoding="utf-8")
    # Say what a language actually holds: `en` is a worked example until the
    # checklist in README.md is done, and a three-page PDF should not look like
    # a manual.
    print(f"{out_html.relative_to(ROOT)}: {count} chapters, {len(doc)} bytes")
    if args.lang != "pl":
        print(f"  ({args.lang}: this is a partial edition -- see docs/manual/README.md)")
    if args.html_only:
        return 0
    chrome = find_chrome(args.chrome)
    if not chrome:
        print("no Chrome/Chromium/Edge found -- the HTML is built; install a browser "
              "or pass --chrome PATH (or $CROSSPAD_CHROME) for the PDF. "
              "Re-run with --html-only to skip this.")
        return 1
    out_pdf = ROOT / spec["pdf"]
    subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={out_pdf}", out_html.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"{out_pdf.relative_to(ROOT)}: {out_pdf.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
