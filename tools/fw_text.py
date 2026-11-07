"""Can the firmware's fonts draw this text?

The fonts carry exactly crosspad-gui tools/fonts/generate.py TEXT_RANGE (ASCII, Latin-1,
Latin Extended-A with holes, a few punctuation marks); anything else draws as
a box on the CrossPad. Profile text reaches the screen verbatim, so CI checks it.
"""
from __future__ import annotations

import importlib.util
from functools import lru_cache
from pathlib import Path

DEL = 0x7F   # inside the font range, but a control character, never a name
GENERATE = (Path(__file__).resolve().parent.parent / "components" / "crosspad-gui"
            / "tools" / "fonts" / "generate.py")


@lru_cache(maxsize=1)
def _ranges() -> tuple[tuple[int, int], ...]:
    spec = importlib.util.spec_from_file_location("crosspad_font_generate", GENERATE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    out = []
    for part in mod.TEXT_RANGE.split(","):
        lo, _, hi = part.partition("-")
        out.append((int(lo, 16), int(hi or lo, 16)))
    return tuple(out)


def missing_glyphs(text: str) -> str:
    """The characters of `text` the fonts lack, sorted, each once ("" when it all draws)."""
    drawable = lambda ch: ord(ch) != DEL and any(lo <= ord(ch) <= hi for lo, hi in _ranges())
    return "".join(sorted({ch for ch in text if not drawable(ch)}))
