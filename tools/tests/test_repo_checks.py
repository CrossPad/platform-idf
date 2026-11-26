"""tools/check_sdkconfig.py, check_image_budget.py, check_conventions.py."""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_conventions
import check_image_budget
import check_sdkconfig


def repo_copy(tmp_path):
    (tmp_path / "config").mkdir()
    shutil.copy(ROOT / "config" / "sdkconfig_invariants.json", tmp_path / "config")
    for name in ("sdkconfig.defaults", "sdkconfig.v1", "sdkconfig.v2"):
        shutil.copy(ROOT / name, tmp_path / name)
    return tmp_path


def test_the_checked_in_sdkconfigs_hold():
    assert check_sdkconfig.problems(ROOT) == []


def test_a_revision_that_loses_a_value_is_named(tmp_path):
    repo = repo_copy(tmp_path)
    f = repo / "sdkconfig.v2"
    f.write_text(f.read_text().replace("CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y",
                                       "# CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB is not set"))
    found = check_sdkconfig.problems(repo)
    assert len(found) == 1 and found[0].startswith("sdkconfig.v2: expected `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`")


def test_defaults_may_be_silent_but_not_disagree(tmp_path):
    repo = repo_copy(tmp_path)
    f = repo / "sdkconfig.defaults"
    f.write_text(f.read_text().replace("CONFIG_LV_CACHE_DEF_SIZE=196608", "CONFIG_LV_CACHE_DEF_SIZE=614400"))
    found = check_sdkconfig.problems(repo)
    assert [x.split(":")[0] for x in found] == ["sdkconfig.defaults"]


def test_blend_code_in_iram_is_refused_in_defaults(tmp_path):
    repo = repo_copy(tmp_path)
    with (repo / "sdkconfig.defaults").open("a") as f:
        f.write("CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y\n")
    found = check_sdkconfig.problems(repo)
    assert len(found) == 1 and "must not be set here" in found[0]


SIZE_A = """build_v2/CrossPad.elf  :
section                   size         addr
.iram0.text             100911   1077380100
.dram0.bss               90752   1070241320
.flash.text            {text}   1107296288
.flash.rodata           770772   1009189216
.ext_ram.dummy         {ext}   1006632992
Total                 72255721
"""


def test_an_image_within_its_pages_passes_and_reports_headroom():
    sections = check_image_budget.parse_size_output(SIZE_A.format(text=2541260, ext=3342304))
    lines, over = check_image_budget.report(sections, 65536, 51)
    assert not over
    assert "PSRAM pages: 51 (budget 51)" in lines
    assert ".flash.text headroom to its next page: 14612 B" in lines


def test_one_more_page_is_over_budget():
    sections = check_image_budget.parse_size_output(SIZE_A.format(text=2541260, ext=3342304 + 65536))
    lines, over = check_image_budget.report(sections, 65536, 51)
    assert over and lines[-1].startswith("OVER BUDGET")


def test_a_revision_without_a_budget_only_reports():
    sections = check_image_budget.parse_size_output(SIZE_A.format(text=2541260, ext=9999999))
    _, over = check_image_budget.report(sections, 65536, None)
    assert not over


def test_the_budget_file_lists_pages_per_revision():
    budget = json.loads((ROOT / "config" / "image_budget.json").read_text())
    assert budget["page_bytes"] == 65536
    assert all(isinstance(r["ext_ram_pages"], int) for r in budget["revisions"].values())


def test_the_tree_follows_the_conventions():
    assert check_conventions.findings(ROOT) == []


def write(repo, rel, text):
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return rel


def test_each_convention_is_caught(tmp_path):
    files = [
        write(tmp_path, "main/foo.cpp", "#ifdef CONFIG_BSP_BOARD_REV_V2\n#endif\n"),
        write(tmp_path, "main/bar.c", "static void f(void) __attribute__((constructor));\n"),
        write(tmp_path, "main/baz.cpp", "static AppRegistrar reg(\"x\");\n"),
        write(tmp_path, "main/app/thing/CMakeLists.txt", "set(THING_SOURCES a.cpp PARENT_SCOPE)\n"),
        write(tmp_path, "components/bsp/crosspad/include/crosspad_s3.h", "#if defined(CONFIG_BSP_BOARD_REV_V2)\n#endif\n"),
        write(tmp_path, "main/ok.cpp", "// CONFIG_BSP_BOARD_REV_V2 in a comment\n#include \"crosspad/app/AppRegistrar.hpp\"\n"),
    ]
    found = check_conventions.findings(tmp_path, files)
    assert [f.split(": ")[1].rstrip(":") for f in found] == ["board-rev", "constructor", "constructor", "parent-scope"]
    assert found[0].startswith("main/foo.cpp:1:")
