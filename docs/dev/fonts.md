# Fonts

Contributor notes: Montserrat with Latin Extended, FATFS in UTF-8. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Fonts (Polish, and every other European name)

LVGL ships Montserrat as ASCII plus the FontAwesome symbols, so a kit called
"ŚWIĘTE SŁOWA" drew as boxes. crosspad-gui's
`src/fonts/lv_font_montserrat_{10,12,14,16,18,24}.c` replace LVGL's copies
**under the same symbol names** — same TTF, same bpp, same `LV_SYMBOL_*` list,
plus Latin-1 Supplement and Latin Extended-A — so none of the ~240 call sites
change and no app picks a special font; the firmware and crosspad-pc take the
same files from the submodule (12 is off in `sdkconfig.v*` and compiles to
nothing here; the simulator uses it). crosspad-gui's `CMakeLists.txt` drops
LVGL's own sources from the `lvgl__lvgl` target
(`cmake/crosspad_gui_fonts.cmake`, duplicate symbols otherwise) and forces each
enabled size undefined with `-u`: its archive is scanned before LVGL's, and
LVGL's default theme and later archives reference the fonts too.

Regenerate with `python3 components/crosspad-gui/tools/fonts/generate.py`
(needs node; fetches lv_font_conv via npx). The range has deliberate holes: Å å
Ů ů and the Latvian cedilla letters raise `line_height` by 2 px at every size on
their own, so they are left out and everything else in both blocks is in. What
remains costs one pixel per line (14 px: 16 → 17) and ~105 kB of flash.
crosspad-gui's `tools/fonts/` carries the TTF, the FontAwesome WOFF and both
licences; `tools/fw_text.py` (profile text in CI) reads its range from there,
so the CI matrix job checks the submodules out.

**FATFS speaks UTF-8** (`CONFIG_FATFS_API_ENCODING_UTF_8`, in
`sdkconfig.defaults` and both `sdkconfig.v*`). The default ANSI/OEM encoding
transcodes every filename to code page 437, which has no Polish letters: a
folder named `Żółć` came back with '?' where the diacritics were and could not
be opened. Kit *display* names were never affected — those come from the kit's
JSON as UTF-8 — only paths.

Before/after evidence for anything that touches fonts, styles or layout: the
`before-after` skill in crosspad-mcp (per-app simulator screenshots plus an
`LVGL_STATS` benchmark on the board).
