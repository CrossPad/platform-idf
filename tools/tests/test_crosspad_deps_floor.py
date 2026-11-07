"""main/include/crosspad_deps.h: same MAJOR and at least the declared triple, or #error."""
import shutil, subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CXX = shutil.which("g++") or shutil.which("c++")


def compile_with(tmp_path, core, gui):
    for sub, name, prefix, v in (("crosspad", "CrosspadCoreVersion.hpp", "CROSSPAD_CORE_VERSION", core),
                                 ("crosspad-gui", "CrosspadGuiVersion.hpp", "CROSSPAD_GUI_VERSION", gui)):
        d = tmp_path / sub
        d.mkdir(exist_ok=True)
        (d / name).write_text("#pragma once\n" + "".join(
            f"#define {prefix}_{p} {x}\n" for p, x in zip(("MAJOR", "MINOR", "PATCH"), v)))
    src = tmp_path / "t.cpp"
    src.write_text('#include "crosspad_deps.h"\n')
    return subprocess.run([CXX, "-fsyntax-only", "-Werror=cpp", "-I", str(ROOT / "main" / "include"),
                           "-I", str(tmp_path), str(src)], capture_output=True, text=True)


pytestmark = pytest.mark.skipif(CXX is None, reason="no C++ compiler on this host")


@pytest.mark.parametrize("core,gui", [((2, 0, 0), (2, 0, 0)), ((2, 7, 2), (2, 0, 0)),
                                      ((2, 0, 1), (2, 3, 0))])
def test_at_or_above_the_floor_builds_without_a_warning(tmp_path, core, gui):
    r = compile_with(tmp_path, core, gui)
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("core,gui,what", [((1, 99, 9), (2, 0, 0), "crosspad-core"),
                                           ((3, 0, 0), (2, 0, 0), "crosspad-core"),
                                           ((2, 0, 0), (1, 99, 9), "crosspad-gui"),
                                           ((2, 0, 0), (3, 0, 0), "crosspad-gui")])
def test_below_the_floor_or_another_major_is_an_error(tmp_path, core, gui, what):
    r = compile_with(tmp_path, core, gui)
    assert r.returncode != 0 and what in r.stderr
