#!/usr/bin/env python3
"""The CrossPad user manual: chapters/*.html -> build/*.full.html -> PDF.

Every file in chapters/ is one <section>, taken in name order. 00-cover is
the cover; 01-toc is rebuilt here from the <h1 id> of every chapter after it,
so a chapter added or renamed lands in the contents by itself. style.css is
inlined. The PDF is printed by headless Chrome (CSS paged media: A4, page
numbers in the footer, a chapter a page).

usage: python3 docs/manual/build.py [--html-only]
"""
import html
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
CHAPTERS = ROOT / "chapters"
OUT_HTML = ROOT / "build" / "crosspad-instrukcja.full.html"
OUT_PDF = ROOT / "CrossPad-instrukcja.pdf"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def toc(chapters):
    rows = []
    n = 0
    for text in chapters:
        m = re.search(r'<h1 id="([^"]+)">(.*?)</h1>', text, re.S)
        if not m:
            continue
        n += 1
        title = re.sub(r"<[^>]+>", "", m.group(2)).strip()
        rows.append(f'<div>{n}. <a href="#{m.group(1)}">{html.escape(title, quote=False)}</a></div>')
    return ('<section class="chapter">\n<h1>Spis treści</h1>\n<div class="toc">\n'
            + "\n".join(rows) + "\n</div>\n</section>\n")


def main():
    files = sorted(CHAPTERS.glob("*.html"))
    texts = [f.read_text(encoding="utf-8") for f in files]
    body = []
    for f, t in zip(files, texts):
        if f.name.startswith("01-toc"):
            body.append(toc([x for g, x in zip(files, texts) if g.name > f.name]))
        else:
            body.append(t)
    # every internal link must point at an id that exists
    ids = set(re.findall(r'id="([^"]+)"', "".join(body)))
    missing = sorted(set(re.findall(r'href="#([^"]+)"', "".join(body))) - ids)
    if missing:
        sys.exit("broken links: " + ", ".join(missing))
    # chapters say img/x.png; the page is printed from build/
    body = [b.replace('src="img/', 'src="../img/') for b in body]
    style = (ROOT / "style.css").read_text(encoding="utf-8")
    doc = ('<!doctype html>\n<html lang="pl">\n<head>\n<meta charset="utf-8">\n'
           "<title>CrossPad — instrukcja obsługi</title>\n<style>\n" + style + "</style>\n</head>\n<body>\n\n"
           + "\n".join(body) + "\n</body>\n</html>\n")
    OUT_HTML.parent.mkdir(exist_ok=True)
    OUT_HTML.write_text(doc, encoding="utf-8")
    print(f"{OUT_HTML.relative_to(ROOT)}: {len(files)} files, {len(doc)} bytes")
    if "--html-only" in sys.argv:
        return
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={OUT_PDF}", OUT_HTML.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"{OUT_PDF.relative_to(ROOT)}: {OUT_PDF.stat().st_size} bytes")


if __name__ == "__main__":
    main()
