#!/usr/bin/env python3
"""Validate editor/icons/*.svg against the icon spec (see editor/icons/README.md)."""
import json, re, sys, xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / "editor" / "icons"
NS = "{http://www.w3.org/2000/svg}"
ELEMS = {"svg", "g", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon"}
GEOM = {"path": {"d"}, "rect": {"x", "y", "width", "height", "rx", "ry"}, "circle": {"cx", "cy", "r"},
        "ellipse": {"cx", "cy", "rx", "ry"}, "line": {"x1", "y1", "x2", "y2"},
        "polyline": {"points"}, "polygon": {"points"}, "g": set()}
COMMON = {"transform", "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width"}
ROOT_ATTRS = {"xmlns", "width", "height", "viewBox", "fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin"}
BADGES = {"info", "warning", "error"}
errors = []
def err(n, m): errors.append(f"{n}.svg: {m}")

def enum_names():
    src = (ROOT / "editor/src/icons.h").read_text()
    body = re.search(r"enum class Icon\s*\{(.*?)\};", src, re.S).group(1)
    body = re.sub(r"//.*", "", body)
    return [re.sub(r"(?<!^)(?=[A-Z])", "_", t).lower() for t in re.findall(r"[A-Za-z0-9]+", body)]

# coverage
manifest = json.loads((ICONS / "manifest.json").read_text())
listed = [i["name"] for g in manifest for i in g["icons"]]
files = sorted(p.stem for p in ICONS.glob("*.svg"))
enum = enum_names()
REQUIRED_NEW = """save folder_open undo redo copy paste duplicate export import package settings_sliders palette keyboard
console hierarchy inspector files lock unlock link unlink filter sort more_horizontal drag_handle check minus maximize
restore fullscreen fullscreen_exit audio_file scene_file shader_file material particles physics animation light_point
light_spot light_directional orthographic perspective snap bug help""".split()
for n in enum + REQUIRED_NEW:
    if n not in files: err(n, "missing (required by enum or UI list)")
for n in files:
    if n not in listed: err(n, "not listed in manifest.json")
for n in listed:
    if n not in files: err(n, "listed in manifest.json but file missing")
if len(listed) != len(set(listed)): errors.append("manifest.json has duplicates")

def color_ok(v): return v in ("currentColor", "none")

for n in files:
    p = ICONS / f"{n}.svg"
    if not re.fullmatch(r"[a-z0-9]+(_[a-z0-9]+)*", n): err(n, "name is not snake_case")
    txt = p.read_text()
    if len(txt) > 3000: err(n, f"too large ({len(txt)} bytes)")
    if re.search(r"<(style|text|filter|mask|clipPath|image|defs|metadata|title)|class=|style=|<!--|<\?xml", txt):
        err(n, "forbidden construct")
    try: root = ET.fromstring(txt)
    except ET.ParseError as e: err(n, f"XML error {e}"); continue
    if root.tag != NS + "svg": err(n, "root is not svg"); continue
    for a in root.attrib:
        if a not in ROOT_ATTRS: err(n, f"root attribute {a} not allowed")
    if root.get("viewBox") != "0 0 24 24": err(n, "viewBox must be 0 0 24 24")
    if root.get("fill") != "none": err(n, 'root fill must be "none"')
    if root.get("stroke") != "currentColor": err(n, 'root stroke must be "currentColor"')
    if root.get("stroke-width") != "1.75": err(n, "root stroke-width must be 1.75")
    if root.get("stroke-linecap") != "round" or root.get("stroke-linejoin") != "round": err(n, "round caps/joins required")
    soft = 0; shapes = 0
    for el in root.iter():
        t = el.tag.replace(NS, "")
        if el is root: continue
        if t not in ELEMS: err(n, f"element <{t}> not allowed"); continue
        shapes += t != "g"
        ok = GEOM[t] | COMMON
        for a, v in el.attrib.items():
            if a not in ok: err(n, f"<{t}> attribute {a} not allowed")
            if a in ("fill", "stroke") and not color_ok(v): err(n, f"color {v} not allowed")
            if a == "stroke-width" and v != "1.75": err(n, "stroke-width override")
            if a == "fill-opacity" and v != "0.18": err(n, "fill-opacity must be 0.18")
            if a == "fill-opacity": soft += 1
            if a == "transform" and not re.fullmatch(r"(rotate\([-\d. ]+\)|translate\([-\d. ]+\)|scale\([-\d. ]+\))( (rotate|translate|scale)\([-\d. ]+\))*", v):
                err(n, f"transform {v} not allowed")
        if el.get("fill") == "currentColor" and el.get("fill-opacity") is None and n not in BADGES and n != "play":
            err(n, "solid fill only allowed for play and badges")
    if soft > (3 if n in BADGES else 1): err(n, f"{soft} soft-fill layers (max 1)")
    if n in BADGES:
        if 'fill-rule="evenodd"' not in txt: err(n, "badge needs fill-rule evenodd")
    if shapes == 0: err(n, "empty icon")

if errors:
    print("\n".join(errors)); print(f"\nFAILED: {len(errors)} problem(s)"); sys.exit(1)
print(f"OK: {len(files)} icons, enum values covered: {len(enum)}")
