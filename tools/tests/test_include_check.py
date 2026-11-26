import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import include_check as ic  # noqa: E402


def scan(text):
    return ic.scan_text(text)


def test_a_plain_repeat_is_definite():
    d, p = scan('#include <atomic>\nint x;\n#include <atomic>\n')
    assert [(e["header"], e["first_line"], e["line"]) for e in d] == [("atomic", 1, 3)]
    assert p == []


def test_the_two_arms_of_a_conditional_are_not_a_repeat():
    # This is the whole reason the checker tracks context: a grep would call
    # these two dead lines and one of them is load-bearing.
    text = ('#ifdef BOARD_V1\n#include "pinout_v1.h"\n#endif\n'
            '#ifdef BOARD_V2\n#include "pinout_v1.h"\n#endif\n')
    d, p = scan(text)
    assert d == []
    assert len(p) == 1 and p[0]["first_context"] != p[0]["context"]


def test_the_same_arm_twice_is_dead_even_when_nested():
    d, p = scan('#if A\n#if B\n#include <z.h>\n#endif\n#endif\n'
                '#if A\n#if B\n#include <z.h>\n#endif\n#endif\n')
    assert len(d) == 1 and d[0]["header"] == "z.h"


def test_endif_restores_the_context():
    text = ('#ifdef X\n#include <a.h>\n#endif\n#include <a.h>\n')
    d, p = scan(text)
    # The second one is at file scope, the first inside X: possible, not definite.
    assert d == [] and len(p) == 1


def test_commented_out_include_does_not_count_and_trailing_comment_does_not_break_it():
    d, p = scan('// #include <gone.h>\n#include <kept.h>   // for the caps\n#include <kept.h>\n')
    assert [e["header"] for e in d] == ["kept.h"]


def test_angle_and_quote_forms_of_the_same_header_are_the_same_header():
    d, _ = scan('#include <sys/stat.h>\n#include "sys/stat.h"\n')
    assert len(d) == 1


def test_build_and_vendored_trees_are_skipped_at_any_depth():
    ex = list(ic.DEFAULT_EXCLUDES)
    assert ic.is_excluded(["build_v2_pc", "esp-idf"], ex)
    assert ic.is_excluded(["main", "build_v2"], ex)
    assert ic.is_excluded(["components", "crosspad-core", "src"], ex)
    assert ic.is_excluded(["components", "crosspad-gui", "src", "x.cpp"], ex)
    assert not ic.is_excluded(["main", "tests"], ex)
    assert not ic.is_excluded(["components", "bsp", "crosspad"], ex)
    assert not ic.is_excluded(["components", "crosspad-platform-idf"], ex)


def test_submodules_are_skipped_because_they_are_other_repos(tmp_path):
    (tmp_path / ".gitmodules").write_text(
        '[submodule "core"]\n\tpath = components/crosspad-core\n'
        '[submodule "s"]\n\tpath = components/crosspad-sampler\n', encoding="utf-8")
    subs = ic.submodule_paths(str(tmp_path))
    assert "components/crosspad-sampler" in subs
    ex = list(ic.DEFAULT_EXCLUDES) + subs
    assert ic.is_excluded(["components", "crosspad-sampler", "src"], ex)


def test_the_bench_files_this_branch_cleaned(tmp_path):
    # The shape of the two removals: a repeat on adjacent lines, and a late
    # include pair in the middle of a large file.
    d, _ = scan('#include "esp_heap_caps.h"\n#include "esp_heap_caps.h"\n')
    assert len(d) == 1
    d2, _ = scan('#include <atomic>\n#include "esp_timer.h"\n' + "x\n" * 50 +
                 '#include <atomic>\n#include "esp_timer.h"\n')
    assert [e["line"] for e in d2] == [53, 54]
    assert [e["first_line"] for e in d2] == [1, 2]


def test_main_exits_one_when_something_is_definite(tmp_path, capsys):
    bad = tmp_path / "a.cpp"
    bad.write_text('#include <z.h>\n#include <z.h>\n', encoding="utf-8")
    assert ic.main([str(bad)]) == 1
    out = capsys.readouterr().out
    assert "definite duplicates" in out and "z.h" in out
    good = tmp_path / "b.cpp"
    good.write_text('#include <z.h>\nint x;\n', encoding="utf-8")
    assert ic.main([str(good)]) == 0
    assert ic.main([str(tmp_path / "nope-nothing")]) == 2
