#!/usr/bin/env python3
"""
Auto-generate architecture and dependency diagrams from the actual project structure.

Parses CMakeLists.txt files, #include directives, and component layout to produce:
  - Component dependency graph (from REQUIRES / PRIV_REQUIRES)
  - Include dependency graph (from #include "..." directives)
  - Architecture layer diagram (predefined layer classification)

Outputs Mermaid markdown, Graphviz DOT, optional SVG, and a self-contained HTML viewer.

Usage:
    python generate_diagrams.py [--project-root DIR] [--output-dir DIR]
                                [--show-idf] [--no-html] [--no-dot] [--verbose]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# ────────────────────────── Configuration ──────────────────────────

ESP_IDF_COMPONENTS = {
    "freertos", "driver", "esp_event", "esp_timer", "esp_common",
    "esp_lcd", "esp_psram", "esp_partition", "nvs_flash", "fatfs",
    "wear_levelling", "efuse", "tinyusb", "perfmon", "spiffs",
    "esp_log", "esp_system", "heap", "soc", "hal", "vfs", "usb",
    "esp_delta_ota",
}

# Components from the managed_components / component registry (fetched, not project-owned)
MANAGED_COMPONENT_PREFIXES = ("espressif__", "bblanchon__")

LAYER_MAP = {
    # Main entry point
    "main":                         "Main",
    # Apps
    "coop":                         "Apps",
    "example":                      "Apps",
    "update":                       "Apps",
    # GUI
    "crosspad-gui":                 "GUI",
    # Platform adapters
    "crosspad-platform-idf":        "Platform",
    "stm32_hal":                    "Platform",
    # Core portable library
    "crosspad-core":                "Core",
    # BSP / Drivers
    "crosspad":                     "BSP",
    "lcd_touch_ft6x36":             "BSP",
    "lsm6dso":                      "BSP",
    # MIDI / USB
    "arduino_midi_library":         "MIDI & USB",
    "midi_esp_tinyusb_transport":   "MIDI & USB",
    "usb_device_uac":               "MIDI & USB",
    # Adapters / utility
    "arduino_json_file_adapter":    "Utility",
}

LAYER_ORDER = ["Main", "Apps", "GUI", "Platform", "Core", "BSP", "MIDI & USB", "Utility", "Managed", "ESP-IDF"]

LAYER_COLORS = {
    "Main":       {"fill": "#0f2618", "stroke": "#238636", "text": "#7ee787"},
    "Apps":       {"fill": "#0f2618", "stroke": "#238636", "text": "#7ee787"},
    "GUI":        {"fill": "#1f0d2a", "stroke": "#8957e5", "text": "#d2a8ff"},
    "Platform":   {"fill": "#152210", "stroke": "#3fb950", "text": "#7ee787"},
    "Core":       {"fill": "#0d2039", "stroke": "#1f6feb", "text": "#79c0ff"},
    "BSP":        {"fill": "#1c1a26", "stroke": "#6e40c9", "text": "#d2a8ff"},
    "MIDI & USB": {"fill": "#250d1a", "stroke": "#f85149", "text": "#ff7b72"},
    "Utility":    {"fill": "#261e0d", "stroke": "#d29922", "text": "#e3b341"},
    "Managed":    {"fill": "#1a1a2e", "stroke": "#6e40c9", "text": "#d2a8ff"},
    "ESP-IDF":    {"fill": "#1a2332", "stroke": "#3d6699", "text": "#79c0ff"},
}


# ────────────────────────── Data Model ──────────────────────────

@dataclass
class ComponentInfo:
    name: str
    cmake_path: str
    include_dirs: List[str] = field(default_factory=list)
    source_files: List[str] = field(default_factory=list)
    requires: List[str] = field(default_factory=list)
    priv_requires: List[str] = field(default_factory=list)
    layer: str = ""


# ────────────────────────── CMake Parsing ──────────────────────────

def extract_register_block(text: str) -> Optional[str]:
    """Extract the full argument block from idf_component_register(...)."""
    match = re.search(r'idf_component_register\s*\(', text)
    if not match:
        return None
    start = match.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
        i += 1
    return text[start:i - 1]


def extract_keyword_list(block: str, keyword: str) -> List[str]:
    """Extract a keyword section (e.g. REQUIRES, INCLUDE_DIRS) from the register block."""
    # Keywords that can terminate a section
    all_keywords = {"SRCS", "SRC_DIRS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
                    "REQUIRES", "PRIV_REQUIRES", "LDFRAGMENTS", "EMBED_FILES",
                    "EMBED_TXTFILES", "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE"}
    pattern = r'\b' + re.escape(keyword) + r'\b'
    match = re.search(pattern, block)
    if not match:
        return []
    rest = block[match.end():]
    # Find the next keyword or end of block
    tokens = []
    for token in rest.split():
        # Remove comments
        if token.startswith('#'):
            break
        if token in all_keywords:
            break
        # Strip quotes
        token = token.strip('"').strip("'")
        if token:
            tokens.append(token)
    return tokens


def parse_cmake_register(cmake_path: str, verbose: bool = False) -> Optional[ComponentInfo]:
    """Parse a CMakeLists.txt file and extract component registration info."""
    try:
        with open(cmake_path, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError:
        return None

    block = extract_register_block(text)
    if block is None:
        return None

    cmake_dir = os.path.dirname(cmake_path)

    # Determine component name from directory structure
    # components/bsp/crosspad/CMakeLists.txt → "crosspad"
    # components/crosspad-core/CMakeLists.txt → "crosspad-core"
    # main/CMakeLists.txt → "main"
    name = os.path.basename(cmake_dir)

    raw_requires = extract_keyword_list(block, "REQUIRES")
    raw_priv_requires = extract_keyword_list(block, "PRIV_REQUIRES")
    raw_include_dirs = extract_keyword_list(block, "INCLUDE_DIRS")

    # Filter out unresolvable CMake variable references from dependency lists
    requires = [r for r in raw_requires if '${' not in r]
    priv_requires = [r for r in raw_priv_requires if '${' not in r]

    # Resolve include dirs to absolute paths
    include_dirs = []
    for d in raw_include_dirs:
        # Handle ${CMAKE_CURRENT_SOURCE_DIR} prefix
        d = d.replace("${CMAKE_CURRENT_SOURCE_DIR}/", "")
        d = d.replace("${CMAKE_CURRENT_SOURCE_DIR}", ".")
        # Skip other unresolvable variable references
        if '${' in d:
            continue
        abs_d = os.path.normpath(os.path.join(cmake_dir, d))
        if os.path.isdir(abs_d):
            include_dirs.append(abs_d)
        elif verbose:
            print(f"  [warn] Include dir not found: {abs_d} (from {cmake_path})")

    # Collect source files
    srcs = extract_keyword_list(block, "SRCS")
    source_files = []
    for s in srcs:
        abs_s = os.path.normpath(os.path.join(cmake_dir, s))
        if os.path.isfile(abs_s):
            source_files.append(abs_s)

    # Also check for GLOB_RECURSE patterns in the cmake text
    for glob_match in re.finditer(r'file\s*\(\s*GLOB_RECURSE\s+\w+\s+"([^"]+)"\s*\)', text):
        glob_pattern = glob_match.group(1)
        # Resolve ${CMAKE_CURRENT_SOURCE_DIR}
        glob_pattern = glob_pattern.replace("${CMAKE_CURRENT_SOURCE_DIR}/", "")
        glob_pattern = glob_pattern.replace("${CMAKE_CURRENT_SOURCE_DIR}", ".")
        glob_dir = os.path.normpath(os.path.join(cmake_dir, os.path.dirname(glob_pattern)))
        ext = os.path.splitext(glob_pattern)[1]
        if os.path.isdir(glob_dir) and ext:
            for root, _, files in os.walk(glob_dir):
                for fn in files:
                    if fn.endswith(ext):
                        source_files.append(os.path.normpath(os.path.join(root, fn)))

    # Assign layer
    layer = LAYER_MAP.get(name, "")
    if not layer:
        if any(name.startswith(p) for p in MANAGED_COMPONENT_PREFIXES):
            layer = "Managed"
        elif name in ESP_IDF_COMPONENTS:
            layer = "ESP-IDF"
        else:
            layer = "Utility"

    comp = ComponentInfo(
        name=name,
        cmake_path=cmake_path,
        include_dirs=include_dirs,
        source_files=source_files,
        requires=requires,
        priv_requires=priv_requires,
        layer=layer,
    )

    if verbose:
        print(f"  Component: {name}")
        print(f"    REQUIRES: {requires}")
        print(f"    PRIV_REQUIRES: {priv_requires}")
        print(f"    INCLUDE_DIRS: {len(include_dirs)}")
        print(f"    Sources: {len(source_files)}")
        print(f"    Layer: {layer}")

    return comp


# ────────────────────────── Discovery ──────────────────────────

def discover_components(root: str, verbose: bool = False) -> Dict[str, ComponentInfo]:
    """Walk the project to find all CMakeLists.txt files with idf_component_register."""
    components: Dict[str, ComponentInfo] = {}
    search_dirs = [
        os.path.join(root, "components"),
        os.path.join(root, "main"),
    ]

    if verbose:
        print("Discovering components...")

    for search_dir in search_dirs:
        if not os.path.isdir(search_dir):
            continue
        for dirpath, dirnames, filenames in os.walk(search_dir):
            if "CMakeLists.txt" in filenames:
                cmake_path = os.path.join(dirpath, "CMakeLists.txt")
                try:
                    with open(cmake_path, 'r', encoding='utf-8', errors='replace') as f:
                        content = f.read()
                except OSError:
                    continue
                if 'idf_component_register' in content:
                    comp = parse_cmake_register(cmake_path, verbose)
                    if comp and comp.name not in components:
                        components[comp.name] = comp

    if verbose:
        print(f"Found {len(components)} components\n")

    return components


# ────────────────────────── Include Graph ──────────────────────────

def build_include_map(components: Dict[str, ComponentInfo]) -> Dict[str, str]:
    """Map relative header paths to their owning component."""
    header_to_component: Dict[str, str] = {}
    for comp in components.values():
        for inc_dir in comp.include_dirs:
            if not os.path.isdir(inc_dir):
                continue
            for root_dir, _, files in os.walk(inc_dir):
                for fn in files:
                    if fn.endswith(('.h', '.hpp', '.hxx')):
                        abs_path = os.path.join(root_dir, fn)
                        rel_path = os.path.relpath(abs_path, inc_dir).replace('\\', '/')
                        if rel_path not in header_to_component:
                            header_to_component[rel_path] = comp.name
    return header_to_component


def parse_includes(source_file: str) -> List[str]:
    """Extract #include "..." paths from a source file."""
    includes = []
    try:
        with open(source_file, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
                if m:
                    includes.append(m.group(1).replace('\\', '/'))
    except OSError:
        pass
    return includes


def build_include_graph(
    components: Dict[str, ComponentInfo],
    include_map: Dict[str, str],
    verbose: bool = False,
) -> Dict[Tuple[str, str], int]:
    """Build component-to-component edges from #include directives. Returns edge -> count."""
    edges: Dict[Tuple[str, str], int] = {}

    for comp in components.values():
        for src in comp.source_files:
            includes = parse_includes(src)
            # Also parse headers in include dirs
            for inc_dir in comp.include_dirs:
                if not os.path.isdir(inc_dir):
                    continue
                for root_dir, _, files in os.walk(inc_dir):
                    for fn in files:
                        if fn.endswith(('.h', '.hpp', '.hxx')):
                            includes.extend(parse_includes(os.path.join(root_dir, fn)))

            for inc_path in includes:
                target_comp = include_map.get(inc_path)
                if target_comp and target_comp != comp.name:
                    edge = (comp.name, target_comp)
                    edges[edge] = edges.get(edge, 0) + 1

    if verbose:
        print(f"Include graph: {len(edges)} cross-component edges")

    return edges


# ────────────────────────── Mermaid Helpers ──────────────────────────

def mermaid_id(name: str) -> str:
    """Convert component name to valid Mermaid node ID."""
    return name.replace("-", "_").replace(".", "_")


def mermaid_node(name: str) -> str:
    """Create a Mermaid node definition with display label."""
    mid = mermaid_id(name)
    if mid != name:
        return f'{mid}["{name}"]'
    return mid


# ────────────────────────── Diagram Generators ──────────────────────────

def generate_component_deps_mermaid(
    components: Dict[str, ComponentInfo],
    show_idf: bool = False,
) -> str:
    """Generate component dependency Mermaid graph."""
    lines = ["graph LR"]

    # Group components by layer
    layers: Dict[str, List[str]] = {}
    for comp in components.values():
        layer = comp.layer
        if not show_idf and layer == "ESP-IDF":
            continue
        layers.setdefault(layer, []).append(comp.name)

    # Emit subgraphs
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        lines.append(f'    subgraph {safe_layer}["{layer}"]')
        for name in sorted(layers[layer]):
            lines.append(f'        {mermaid_node(name)}')
        lines.append("    end")

    # Emit edges
    declared_nodes: Set[str] = set()
    for comp in components.values():
        if not show_idf and comp.layer == "ESP-IDF":
            continue
        declared_nodes.add(comp.name)

    for comp in components.values():
        if comp.name not in declared_nodes:
            continue
        for req in comp.requires:
            if req in declared_nodes:
                lines.append(f"    {mermaid_id(comp.name)} --> {mermaid_id(req)}")
        for req in comp.priv_requires:
            if req in declared_nodes:
                lines.append(f"    {mermaid_id(comp.name)} -.-> {mermaid_id(req)}")

    # Styles
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        lines.append(f'    style {safe_layer} fill:{c["fill"]},stroke:{c["stroke"]},color:{c["text"]}')

    return "\n".join(lines)


def generate_include_deps_mermaid(
    components: Dict[str, ComponentInfo],
    include_edges: Dict[Tuple[str, str], int],
    show_idf: bool = False,
) -> str:
    """Generate include dependency Mermaid graph."""
    lines = ["graph LR"]

    # Only include components that participate in edges
    participating: Set[str] = set()
    for (src, dst), _ in include_edges.items():
        participating.add(src)
        participating.add(dst)

    # Filter out ESP-IDF components unless requested
    if not show_idf:
        participating = {n for n in participating
                         if n in components and components[n].layer != "ESP-IDF"}

    # Group by layer
    layers: Dict[str, List[str]] = {}
    for name in participating:
        if name in components:
            layer = components[name].layer
        else:
            layer = "Utility"
        layers.setdefault(layer, []).append(name)

    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        lines.append(f'    subgraph {safe_layer}["{layer}"]')
        for name in sorted(layers[layer]):
            lines.append(f'        {mermaid_node(name)}')
        lines.append("    end")

    # Edges with count labels
    for (src, dst), count in sorted(include_edges.items()):
        if src not in participating or dst not in participating:
            continue
        label = f"|{count} headers|" if count > 1 else ""
        lines.append(f"    {mermaid_id(src)} -->{label} {mermaid_id(dst)}")

    # Styles
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        lines.append(f'    style {safe_layer} fill:{c["fill"]},stroke:{c["stroke"]},color:{c["text"]}')

    return "\n".join(lines)


def generate_architecture_layers_mermaid(
    components: Dict[str, ComponentInfo],
    show_idf: bool = False,
) -> str:
    """Generate top-to-bottom architecture layer diagram."""
    lines = ["graph TB"]

    # Group by layer
    layers: Dict[str, List[str]] = {}
    for comp in components.values():
        if not show_idf and comp.layer == "ESP-IDF":
            continue
        layers.setdefault(comp.layer, []).append(comp.name)

    # Emit subgraphs in order
    prev_layer_id = None
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        lines.append(f'    subgraph {safe_layer}["{layer}"]')
        lines.append("        direction LR")
        for name in sorted(layers[layer]):
            lines.append(f'        {mermaid_node(name)}')
        lines.append("    end")

        # Connect layers top-to-bottom
        if prev_layer_id is not None:
            lines.append(f"    {prev_layer_id} --> {safe_layer}")
        prev_layer_id = safe_layer

    # Styles
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        safe_layer = layer.replace(" ", "_").replace("&", "and")
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        lines.append(f'    style {safe_layer} fill:{c["fill"]},stroke:{c["stroke"]},color:{c["text"]}')

    return "\n".join(lines)


# ────────────────────────── DOT Generators ──────────────────────────

def _dot_escape(name: str) -> str:
    """Escape a component name for DOT node IDs."""
    return name.replace("-", "_").replace(".", "_")


def generate_component_deps_dot(
    components: Dict[str, ComponentInfo],
    show_idf: bool = False,
) -> str:
    """Generate component dependency DOT graph."""
    lines = [
        'digraph component_dependencies {',
        '    rankdir=LR;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '    edge [color="#58a6ff", fontname="Segoe UI", fontsize=9, fontcolor="#8b949e"];',
        '',
    ]

    # Group into subgraphs
    layers: Dict[str, List[str]] = {}
    for comp in components.values():
        if not show_idf and comp.layer == "ESP-IDF":
            continue
        layers.setdefault(comp.layer, []).append(comp.name)

    cluster_idx = 0
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        lines.append(f'    subgraph cluster_{cluster_idx} {{')
        lines.append(f'        label="{layer}";')
        lines.append(f'        style=filled; fillcolor="{c["fill"]}"; color="{c["stroke"]}";')
        lines.append(f'        fontcolor="{c["text"]}"; fontname="Segoe UI"; fontsize=12;')
        for name in sorted(layers[layer]):
            lines.append(f'        {_dot_escape(name)} [label="{name}", fillcolor="{c["fill"]}", color="{c["stroke"]}"];')
        lines.append('    }')
        cluster_idx += 1

    lines.append('')

    # Edges
    declared: Set[str] = set()
    for comp in components.values():
        if not show_idf and comp.layer == "ESP-IDF":
            continue
        declared.add(comp.name)

    for comp in components.values():
        if comp.name not in declared:
            continue
        for req in comp.requires:
            if req in declared:
                lines.append(f'    {_dot_escape(comp.name)} -> {_dot_escape(req)};')
        for req in comp.priv_requires:
            if req in declared:
                lines.append(f'    {_dot_escape(comp.name)} -> {_dot_escape(req)} [style=dashed, color="#8b949e"];')

    lines.append('}')
    return '\n'.join(lines)


def generate_include_deps_dot(
    components: Dict[str, ComponentInfo],
    include_edges: Dict[Tuple[str, str], int],
    show_idf: bool = False,
) -> str:
    """Generate include dependency DOT graph."""
    lines = [
        'digraph include_dependencies {',
        '    rankdir=LR;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '    edge [color="#58a6ff", fontname="Segoe UI", fontsize=9, fontcolor="#8b949e"];',
        '',
    ]

    participating: Set[str] = set()
    for (src, dst), _ in include_edges.items():
        participating.add(src)
        participating.add(dst)
    if not show_idf:
        participating = {n for n in participating
                         if n in components and components[n].layer != "ESP-IDF"}

    layers: Dict[str, List[str]] = {}
    for name in participating:
        layer = components[name].layer if name in components else "Utility"
        layers.setdefault(layer, []).append(name)

    cluster_idx = 0
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        lines.append(f'    subgraph cluster_{cluster_idx} {{')
        lines.append(f'        label="{layer}";')
        lines.append(f'        style=filled; fillcolor="{c["fill"]}"; color="{c["stroke"]}";')
        lines.append(f'        fontcolor="{c["text"]}"; fontname="Segoe UI"; fontsize=12;')
        for name in sorted(layers[layer]):
            lines.append(f'        {_dot_escape(name)} [label="{name}", fillcolor="{c["fill"]}", color="{c["stroke"]}"];')
        lines.append('    }')
        cluster_idx += 1

    lines.append('')

    for (src, dst), count in sorted(include_edges.items()):
        if src not in participating or dst not in participating:
            continue
        label = f' [label="{count}"]' if count > 1 else ''
        lines.append(f'    {_dot_escape(src)} -> {_dot_escape(dst)}{label};')

    lines.append('}')
    return '\n'.join(lines)


def generate_architecture_layers_dot(
    components: Dict[str, ComponentInfo],
    show_idf: bool = False,
) -> str:
    """Generate architecture layer DOT graph."""
    lines = [
        'digraph architecture_layers {',
        '    rankdir=TB;',
        '    bgcolor="#0d1117";',
        '    node [shape=box, style="filled,rounded", fontname="Segoe UI", fontsize=11, fontcolor="#e6edf3"];',
        '    edge [color="#58a6ff", fontname="Segoe UI", fontsize=9];',
        '',
    ]

    layers: Dict[str, List[str]] = {}
    for comp in components.values():
        if not show_idf and comp.layer == "ESP-IDF":
            continue
        layers.setdefault(comp.layer, []).append(comp.name)

    cluster_idx = 0
    layer_ids = []
    for layer in LAYER_ORDER:
        if layer not in layers:
            continue
        if not show_idf and layer == "ESP-IDF":
            continue
        c = LAYER_COLORS.get(layer, LAYER_COLORS["Utility"])
        cid = f"cluster_{cluster_idx}"
        # Use an invisible anchor node for inter-cluster edges
        anchor = f"_anchor_{cluster_idx}"
        lines.append(f'    subgraph {cid} {{')
        lines.append(f'        label="{layer}";')
        lines.append(f'        style=filled; fillcolor="{c["fill"]}"; color="{c["stroke"]}";')
        lines.append(f'        fontcolor="{c["text"]}"; fontname="Segoe UI"; fontsize=13;')
        for name in sorted(layers[layer]):
            lines.append(f'        {_dot_escape(name)} [label="{name}", fillcolor="{c["fill"]}", color="{c["stroke"]}"];')
        lines.append(f'        {anchor} [label="", shape=point, width=0, height=0];')
        lines.append('    }')
        layer_ids.append(anchor)
        cluster_idx += 1

    lines.append('')

    # Connect layers top-to-bottom
    for i in range(len(layer_ids) - 1):
        lines.append(f'    {layer_ids[i]} -> {layer_ids[i + 1]} [style=bold, color="#58a6ff"];')

    lines.append('}')
    return '\n'.join(lines)


# ────────────────────────── HTML Generator ──────────────────────────

def generate_html(diagrams: Dict[str, str], timestamp: str) -> str:
    """Generate self-contained interactive HTML with all diagrams in tabs."""
    tab_defs = [
        ("Component Dependencies", "component_dependencies", "tag-yellow", "dependencies",
         "Parsed from idf_component_register(REQUIRES / PRIV_REQUIRES). Solid = REQUIRES (public), dashed = PRIV_REQUIRES (private)."),
        ("Include Dependencies", "include_dependencies", "tag-purple", "includes",
         "Parsed from #include directives. Edge labels show the number of cross-component header includes."),
        ("Architecture Layers", "architecture_layers", "tag-blue", "layers",
         "Predefined layer classification showing the project's layered architecture from entry point to hardware."),
    ]

    tabs_html = ""
    panels_html = ""

    for idx, (title, key, tag_class, tag_text, description) in enumerate(tab_defs):
        active = " active" if idx == 0 else ""
        tabs_html += f'  <div class="tab{active}" data-idx="{idx}">{title}</div>\n'

        hidden = "" if idx == 0 else ' class="hidden"'
        mermaid_code = diagrams.get(key, "graph LR\n    A-->B")

        panels_html += f"""<div class="panel"{' class="hidden"' if idx > 0 else ''}>
  <div class="card">
    <div class="card-header">
      <h2>{title}</h2>
      <span class="tag {tag_class}">{tag_text}</span>
    </div>
    <div class="card-body">
      <p style="font-size:0.78rem;color:var(--fg2);margin-bottom:12px">{description}</p>
      <div class="mermaid-wrap">
        <div class="mermaid">
{mermaid_code}
        </div>
      </div>
    </div>
    <div class="legend">
      <div class="legend-item"><div class="legend-dot" style="background:#238636"></div>Main / Apps</div>
      <div class="legend-item"><div class="legend-dot" style="background:#8957e5"></div>GUI</div>
      <div class="legend-item"><div class="legend-dot" style="background:#3fb950"></div>Platform</div>
      <div class="legend-item"><div class="legend-dot" style="background:#1f6feb"></div>Core (portable)</div>
      <div class="legend-item"><div class="legend-dot" style="background:#6e40c9"></div>BSP / Managed</div>
      <div class="legend-item"><div class="legend-dot" style="background:#f85149"></div>MIDI &amp; USB</div>
      <div class="legend-item"><div class="legend-dot" style="background:#d29922"></div>Utility</div>
    </div>
  </div>
</div>
"""

    # Fix the hidden attribute on panels (use JS like the original)
    panels_html = panels_html.replace(' class="hidden"', '')

    return f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CrossPad Auto-Generated Diagrams</title>
  <script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
  <style>
    :root {{
      --bg: #0d1117; --bg2: #161b22; --bg3: #1c2128;
      --border: #30363d; --border2: #21262d;
      --fg: #e6edf3; --fg2: #8b949e;
      --blue: #58a6ff; --green: #3fb950; --yellow: #d29922;
      --red: #f85149; --purple: #bc8cff;
      --radius: 8px;
    }}
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{ font-family: -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; background: var(--bg); color: var(--fg); }}
    header {{
      background: var(--bg2); border-bottom: 1px solid var(--border);
      padding: 14px 28px; display: flex; align-items: center; gap: 14px;
      position: sticky; top: 0; z-index: 100;
    }}
    header h1 {{ font-size: 1.15rem; font-weight: 600; }}
    header h1 span {{ color: var(--blue); }}
    .header-badges {{ display: flex; gap: 8px; margin-left: auto; }}
    .badge {{
      font-size: 0.7rem; padding: 3px 8px; border-radius: 12px;
      border: 1px solid var(--border); color: var(--fg2); background: var(--bg);
    }}
    .badge-auto {{ border-color: var(--green); color: var(--green); }}
    .tabs {{
      display: flex; gap: 2px; padding: 0 28px;
      border-bottom: 1px solid var(--border); background: var(--bg);
      overflow-x: auto; scrollbar-width: thin;
    }}
    .tab {{
      padding: 11px 18px; cursor: pointer; font-size: 0.82rem; font-weight: 500;
      color: var(--fg2); border-bottom: 2px solid transparent;
      transition: all 0.12s; user-select: none; white-space: nowrap;
    }}
    .tab:hover {{ color: var(--fg); }}
    .tab.active {{ color: var(--blue); border-bottom-color: var(--blue); }}
    .panel {{ padding: 28px; max-width: 1600px; margin: 0 auto; display: flex; flex-direction: column; gap: 20px; }}
    .panel.hidden {{ display: none; }}
    .card {{
      background: var(--bg2); border: 1px solid var(--border);
      border-radius: var(--radius); overflow: hidden;
    }}
    .card-header {{
      padding: 14px 20px; border-bottom: 1px solid var(--border);
      display: flex; align-items: center; gap: 10px;
    }}
    .card-header h2 {{ font-size: 0.82rem; font-weight: 600; color: var(--fg); }}
    .card-header .tag {{
      font-size: 0.65rem; padding: 2px 7px; border-radius: 10px; font-weight: 500; border: 1px solid;
    }}
    .tag-blue {{ color: var(--blue); border-color: #1f6feb55; background: #1f6feb15; }}
    .tag-green {{ color: var(--green); border-color: #3fb95055; background: #3fb95015; }}
    .tag-purple {{ color: var(--purple); border-color: #8957e555; background: #8957e515; }}
    .tag-yellow {{ color: var(--yellow); border-color: #d2992255; background: #d2992215; }}
    .card-body {{ padding: 20px; }}
    .mermaid-wrap {{
      overflow: hidden; height: 70vh; min-height: 400px; position: relative; cursor: grab;
      border-radius: 4px; background: #0d111799;
    }}
    .mermaid-wrap.dragging {{ cursor: grabbing; }}
    .mermaid-wrap .mermaid {{
      width: 100%; height: 100%; display: flex; align-items: center; justify-content: center;
    }}
    .mermaid-wrap .mermaid svg {{
      shape-rendering: geometricPrecision;
      text-rendering: optimizeLegibility;
    }}
    .zoom-controls {{
      position: absolute; top: 8px; right: 8px; display: flex; gap: 4px; z-index: 10;
    }}
    .zoom-btn {{
      width: 32px; height: 32px; border-radius: 6px; border: 1px solid var(--border);
      background: var(--bg2); color: var(--fg2); font-size: 0.95rem; cursor: pointer;
      display: flex; align-items: center; justify-content: center; transition: all 0.1s;
      font-family: -apple-system, 'Segoe UI', sans-serif; line-height: 1;
    }}
    .zoom-btn:hover {{ background: var(--bg3); color: var(--fg); border-color: var(--fg2); }}
    .zoom-label {{
      height: 32px; padding: 0 10px; border-radius: 6px; border: 1px solid var(--border);
      background: var(--bg); color: var(--fg2); font-size: 0.75rem;
      display: flex; align-items: center; font-family: 'SFMono-Regular', Consolas, monospace;
    }}
    .legend {{ display: flex; flex-wrap: wrap; gap: 16px; padding: 14px 20px; border-top: 1px solid var(--border); }}
    .legend-item {{ display: flex; align-items: center; gap: 7px; font-size: 0.75rem; color: var(--fg2); }}
    .legend-dot {{ width: 10px; height: 10px; border-radius: 3px; flex-shrink: 0; }}
  </style>
</head>
<body>

<header>
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none">
    <rect width="24" height="24" rx="5" fill="#58a6ff" opacity="0.15"/>
    <rect x="4" y="4" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="13" y="4" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="4" y="13" width="7" height="7" rx="1.5" fill="#58a6ff"/>
    <rect x="13" y="13" width="7" height="7" rx="1.5" fill="#3fb950"/>
  </svg>
  <h1><span>CrossPad</span> Auto-Generated Diagrams</h1>
  <div class="header-badges">
    <span class="badge badge-auto">auto-generated {timestamp}</span>
    <span class="badge">ESP32-S3</span>
    <span class="badge">ESP-IDF v5.5</span>
  </div>
</header>

<div class="tabs" id="tabs">
{tabs_html}</div>

{panels_html}

<script>
  mermaid.initialize({{
    startOnLoad: false,
    theme: 'dark',
    themeVariables: {{
      background: '#0d1117',
      primaryColor: '#1f6feb',
      primaryTextColor: '#e6edf3',
      primaryBorderColor: '#30363d',
      lineColor: '#58a6ff',
      secondaryColor: '#161b22',
      tertiaryColor: '#0d1117',
      nodeBorder: '#30363d',
      clusterBkg: '#161b22',
      clusterBorder: '#30363d',
      titleColor: '#e6edf3',
      edgeLabelBackground: '#0d1117',
      fontFamily: "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif",
      fontSize: '16px',
      labelTextColor: '#e6edf3',
      nodeTextColor: '#e6edf3',
    }},
    flowchart: {{ curve: 'basis', padding: 24, nodeSpacing: 50, rankSpacing: 60, htmlLabels: true, wrappingWidth: 200 }},
  }});

  (async () => {{
    await mermaid.run({{ querySelector: '.mermaid' }});

    document.querySelectorAll('.mermaid-wrap').forEach(wrap => {{
      const mermaidDiv = wrap.querySelector('.mermaid');
      if (!mermaidDiv) return;
      const svg = mermaidDiv.querySelector('svg');
      if (!svg) return;

      const bbox = svg.getBBox();
      const pad = 30;
      const fullVB = {{
        x: bbox.x - pad, y: bbox.y - pad,
        w: bbox.width + pad * 2, h: bbox.height + pad * 2
      }};

      svg.setAttribute('viewBox', fullVB.x + ' ' + fullVB.y + ' ' + fullVB.w + ' ' + fullVB.h);
      svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
      svg.removeAttribute('width');
      svg.removeAttribute('height');
      svg.style.width = '100%';
      svg.style.height = '100%';

      const controls = document.createElement('div');
      controls.className = 'zoom-controls';
      controls.innerHTML =
        '<button class="zoom-btn" data-action="in" title="Zoom in">+</button>' +
        '<span class="zoom-label">Fit</span>' +
        '<button class="zoom-btn" data-action="out" title="Zoom out">&minus;</button>' +
        '<button class="zoom-btn" data-action="fit" title="Fit all">&#8596;</button>';
      wrap.appendChild(controls);

      let vb = {{ ...fullVB }};
      const label = controls.querySelector('.zoom-label');
      const MIN_SCALE = 0.15, MAX_SCALE = 10;

      function getZoomLevel() {{ return fullVB.w / vb.w; }}

      function isFitView() {{
        return Math.abs(vb.x - fullVB.x) < 1 && Math.abs(vb.y - fullVB.y) < 1 &&
               Math.abs(vb.w - fullVB.w) < 1 && Math.abs(vb.h - fullVB.h) < 1;
      }}

      function applyVB() {{
        svg.setAttribute('viewBox', vb.x + ' ' + vb.y + ' ' + vb.w + ' ' + vb.h);
        label.textContent = isFitView() ? 'Fit' : Math.round(getZoomLevel() * 100) + '%';
      }}

      function zoomAt(factor, cx, cy) {{
        const newW = vb.w / factor;
        const newH = vb.h / factor;
        const newScale = fullVB.w / newW;
        if (newScale < MIN_SCALE || newScale > MAX_SCALE) return;
        const ratioX = (cx - vb.x) / vb.w;
        const ratioY = (cy - vb.y) / vb.h;
        vb.x = cx - ratioX * newW;
        vb.y = cy - ratioY * newH;
        vb.w = newW;
        vb.h = newH;
        applyVB();
      }}

      function screenToSVG(clientX, clientY) {{
        const rect = svg.getBoundingClientRect();
        const rx = (clientX - rect.left) / rect.width;
        const ry = (clientY - rect.top) / rect.height;
        return {{ x: vb.x + rx * vb.w, y: vb.y + ry * vb.h }};
      }}

      wrap.addEventListener('wheel', e => {{
        e.preventDefault();
        const factor = e.deltaY < 0 ? 1.18 : 1 / 1.18;
        const pt = screenToSVG(e.clientX, e.clientY);
        zoomAt(factor, pt.x, pt.y);
      }}, {{ passive: false }});

      let dragging = false, dragStart = null, vbStart = null;
      wrap.addEventListener('mousedown', e => {{
        if (e.target.closest('.zoom-controls')) return;
        dragging = true;
        wrap.classList.add('dragging');
        dragStart = {{ x: e.clientX, y: e.clientY }};
        vbStart = {{ ...vb }};
        e.preventDefault();
      }});
      window.addEventListener('mousemove', e => {{
        if (!dragging) return;
        const rect = svg.getBoundingClientRect();
        if (rect.width === 0 || rect.height === 0) return;
        const dx = (e.clientX - dragStart.x) / rect.width * vbStart.w;
        const dy = (e.clientY - dragStart.y) / rect.height * vbStart.h;
        vb.x = vbStart.x - dx;
        vb.y = vbStart.y - dy;
        applyVB();
      }});
      window.addEventListener('mouseup', () => {{
        if (!dragging) return;
        dragging = false;
        wrap.classList.remove('dragging');
      }});

      controls.addEventListener('click', e => {{
        const btn = e.target.closest('.zoom-btn');
        if (!btn) return;
        const action = btn.dataset.action;
        const cx = vb.x + vb.w / 2;
        const cy = vb.y + vb.h / 2;
        if (action === 'in')  zoomAt(1.4, cx, cy);
        if (action === 'out') zoomAt(1 / 1.4, cx, cy);
        if (action === 'fit') {{ vb = {{ ...fullVB }}; applyVB(); }}
      }});

      applyVB();
    }});

    const panels = document.querySelectorAll('.panel');
    panels.forEach((p, i) => {{ if (i !== 0) p.classList.add('hidden'); }});
  }})();

  document.getElementById('tabs').addEventListener('click', e => {{
    const tab = e.target.closest('.tab');
    if (!tab) return;
    const idx = +tab.dataset.idx;
    document.querySelectorAll('.tab').forEach((t, i) => t.classList.toggle('active', i === idx));
    document.querySelectorAll('.panel').forEach((p, i) => {{
      p.classList.toggle('hidden', i !== idx);
    }});
  }});
</script>
</body>
</html>'''


# ────────────────────────── SVG Rendering ──────────────────────────

def try_render_svg(dot_files: List[Tuple[str, str]], verbose: bool = False) -> int:
    """Try to render DOT files to SVG using Graphviz. Returns count of rendered files."""
    dot_cmd = shutil.which("dot")
    if not dot_cmd:
        if verbose:
            print("Graphviz 'dot' not found on PATH - skipping SVG rendering")
        return 0

    count = 0
    for dot_path, svg_path in dot_files:
        try:
            subprocess.run(
                [dot_cmd, "-Tsvg", "-o", svg_path, dot_path],
                check=True, capture_output=True, timeout=30,
            )
            count += 1
            if verbose:
                print(f"  Rendered: {svg_path}")
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            if verbose:
                print(f"  Failed to render {dot_path}: {e}")
    return count


# ────────────────────────── Main ──────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Generate CrossPad architecture diagrams")
    parser.add_argument("--project-root", default=".", help="Project root directory")
    parser.add_argument("--output-dir", default=None, help="Output directory (default: docs/generated/)")
    parser.add_argument("--show-idf", action="store_true", help="Include ESP-IDF standard components")
    parser.add_argument("--no-html", action="store_true", help="Skip HTML output")
    parser.add_argument("--no-dot", action="store_true", help="Skip DOT/SVG output")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    project_root = os.path.abspath(args.project_root)
    output_dir = args.output_dir or os.path.join(project_root, "docs", "generated")

    if args.verbose:
        print(f"Project root: {project_root}")
        print(f"Output dir:   {output_dir}")
        print()

    # 1. Discover components
    components = discover_components(project_root, args.verbose)
    if not components:
        print("ERROR: No components found. Is --project-root correct?", file=sys.stderr)
        sys.exit(1)

    # 2. Build include map and graph
    if args.verbose:
        print("\nBuilding include dependency map...")
    include_map = build_include_map(components)
    if args.verbose:
        print(f"  Mapped {len(include_map)} header paths to components")
    include_edges = build_include_graph(components, include_map, args.verbose)

    # 3. Generate diagrams
    if args.verbose:
        print("\nGenerating diagrams...")

    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M")

    comp_mermaid = generate_component_deps_mermaid(components, args.show_idf)
    inc_mermaid = generate_include_deps_mermaid(components, include_edges, args.show_idf)
    arch_mermaid = generate_architecture_layers_mermaid(components, args.show_idf)

    # 4. Write outputs
    os.makedirs(output_dir, exist_ok=True)

    # Mermaid markdown files
    md_files = [
        ("component_dependencies.md", "Component Dependencies", comp_mermaid),
        ("include_dependencies.md", "Include Dependencies", inc_mermaid),
        ("architecture_layers.md", "Architecture Layers", arch_mermaid),
    ]
    for filename, title, mermaid_code in md_files:
        path = os.path.join(output_dir, filename)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f"# {title}\n\n")
            f.write(f"Auto-generated on {timestamp}\n\n")
            f.write("```mermaid\n")
            f.write(mermaid_code)
            f.write("\n```\n")
        if args.verbose:
            print(f"  Wrote: {path}")

    # DOT files
    dot_files_to_render = []
    if not args.no_dot:
        comp_dot = generate_component_deps_dot(components, args.show_idf)
        inc_dot = generate_include_deps_dot(components, include_edges, args.show_idf)
        arch_dot = generate_architecture_layers_dot(components, args.show_idf)

        dot_outputs = [
            ("component_dependencies.dot", comp_dot),
            ("include_dependencies.dot", inc_dot),
            ("architecture_layers.dot", arch_dot),
        ]
        for filename, dot_code in dot_outputs:
            path = os.path.join(output_dir, filename)
            with open(path, 'w', encoding='utf-8') as f:
                f.write(dot_code)
            svg_path = os.path.join(output_dir, filename.replace('.dot', '.svg'))
            dot_files_to_render.append((path, svg_path))
            if args.verbose:
                print(f"  Wrote: {path}")

    # HTML
    if not args.no_html:
        html_content = generate_html({
            "component_dependencies": comp_mermaid,
            "include_dependencies": inc_mermaid,
            "architecture_layers": arch_mermaid,
        }, timestamp)
        html_path = os.path.join(output_dir, "diagrams.html")
        with open(html_path, 'w', encoding='utf-8') as f:
            f.write(html_content)
        if args.verbose:
            print(f"  Wrote: {html_path}")

    # SVG rendering
    svg_count = 0
    if dot_files_to_render:
        svg_count = try_render_svg(dot_files_to_render, args.verbose)

    # Summary
    file_count = len(md_files) + (len(dot_files_to_render) if not args.no_dot else 0) + (1 if not args.no_html else 0) + svg_count
    print(f"\nGenerated {file_count} files in {output_dir}")
    print(f"  Components: {len(components)}")
    print(f"  CMake dependency edges: {sum(len(c.requires) + len(c.priv_requires) for c in components.values())}")
    print(f"  Include dependency edges: {len(include_edges)}")
    if svg_count:
        print(f"  SVGs rendered: {svg_count}")
    elif not args.no_dot:
        print("  SVGs: skipped (Graphviz 'dot' not on PATH)")


if __name__ == "__main__":
    main()
