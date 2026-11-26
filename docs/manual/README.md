# The manual, and its languages

`build.py` in this folder prints the user manual: one `<section>` per file in a
language's chapter directory, taken in name order, with the contents page
regenerated from the `<h1 id>` of every chapter after it, `style.css` inlined,
and the PDF printed by headless Chrome (A4, a chapter a page, page numbers in
the footer).

| language | chapters | command | PDF |
|---|---|---|---|
| Polish (the original, what CI builds) | `chapters/*.html` | `python3 docs/manual/build.py` | `CrossPad-instrukcja.pdf` |
| English (a start, not an edition yet) | `chapters/en/*.html` | `python3 docs/manual/build.py --lang en` | `CrossPad-instructions.pdf` |

The Polish build is untouched by the language support: same directory, same
file list, same output names. `--html-only` stops at the page, which is what to
run when there is no browser: the browser used to be one hard-coded macOS path,
and is now looked for per platform (`--chrome PATH`, then `$CROSSPAD_CHROME`,
then the usual names including `msedge.exe`, which is on every Windows machine).

## The rules a translation follows

A translated chapter is a translation, not a rewrite: same `<section>`, same
`<h1>/<h2>` structure, same `<ul>`s, the same `<span class="lbl">` for words
that appear on the device's screen, the same images (`src="img/…"`, shared —
the screenshots show device UI, which today is English anyway).

Two things are deliberately *not* shared, and are the reason the English build
can fail before the edition is complete:

- **Links between chapters.** `build.py` exits on an `href="#…"` that points at
  an id no chapter in that language defines. That is a feature: a partial
  edition that links to an untranslated chapter would print a broken contents
  page and nobody would notice. Until the target chapter exists, name it in
  words (`the <em>Handling</em> chapter`) and leave the link to restore in an
  HTML comment next to it, as `en/02-wstep.html` does.
- **The version line on the cover and in the tip box**, which is the part of a
  manual that goes stale first. It currently disagrees across the tree: the
  Polish edition still says v1.1.2 (7 November 2026) while v1.1.4 is what
  `main` released. Bump both at a release, in the same PR as the release notes.

## What is translated, and what is left

Translated: `en/00-cover.html`, `en/01-toc.html`, `en/02-wstep.html` (the
welcome chapter, with the reading conventions and the safety list). That is the
worked example: enough to print a real English PDF and to settle the layout, the
terminology and the two rules above.

The remaining 19 Polish chapters, in the order a reader meets them — each is a
file in `chapters/`, copy it to `chapters/en/` with the same name prefix and
translate in place:

| Polish file | chapter |
|---|---|
| `02a-skrot.html` | shortcuts (the one-page summary) |
| `03-budowa.html`, `03a-technologia.html` | what it is, and the technology inside |
| `04-start.html` | first steps |
| `05-obsluga.html` | handling: pads, encoder, touch, the power button |
| `06-kity.html` | kits and the kit browser |
| `07-sampler.html` | Sampler: playing a kit |
| `08-sequencer.html` | Sequencer: rhythms and loops |
| `09-song.html` | Song: a piece made of scenes |
| `10-arrange.html` | Arrange: a piece on tracks |
| `11-recorder.html` | Recorder: recording |
| `12-slicer.html` | Slicer: from a kit loop |
| `13-pitchplay.html` | Pitch Play: an instrument from one sample |
| `14-mixer.html` | Mixer: inputs, outputs, routing |
| `15-padmix.html` | Pad Mixer and DAW Control |
| `16-komputer.html` | CrossPad and a computer |
| `17-midi.html` | MIDI |
| `18-ustawienia.html` | Settings |
| `19-pliki.html` | the microSD card and files |
| `20-aktualizacje.html` | updates |
| `21-problemy.html` | troubleshooting |
| `22-specyfikacja.html` | specification |

A translation of one chapter is a self-contained PR: add the file, and where its
links were replaced by words in the English text before, put the real
`href="#…"` back (the ids are the same in both languages). When the last one
lands, add the English PDF to the docs workflow next to the Polish one — not
before, because a three-page "manual" on the download page is worse than none.
