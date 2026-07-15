#!/usr/bin/env python3
"""Convert Pi-style aircraft silhouette PNGs to tintable alpha masks in flash.

Source layout (same as FlightScnr_Pi):
  assets/aircraft_icons/aircraft-icons.json
  assets/aircraft_icons/*.png

Each PNG is a black silhouette with alpha. We store an 8-bit alpha mask at
ICON_SIDE×ICON_SIDE so firmware can tint to any radar color and rotate by heading.

Usage:
  python tools/aircraft_icons_to_header.py [SRC_DIR] [OUT_HEADER]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow required: pip install Pillow")
    sys.exit(1)

ICON_SIDE = 32
# Soft edges below this are treated as transparent at draw time too.
ALPHA_FLOOR = 24

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SRC = ROOT / "assets" / "aircraft_icons"
DEFAULT_OUT = ROOT / "include" / "data" / "aircraft_icons_lookup.h"

# Stable category IDs (must match C++ IconCategory enum order).
CATEGORY_ORDER = [
    "large-jet-4",
    "large-jet-2",
    "medium-jet",
    "regional-jet",
    "business-jet",
    "turboprop",
    "small-prop-single",
    "small-prop-twin",
    "helicopter",
    "military-helicopter",
    "military-fighter",
    "military-transport",
    "cargo",
    "balloon",
    "airship",
    "drone",
    "military-drone",
    "fighter",
    "glider",
    "unknown",
]

# Draw-size multipliers ×100.
# Pi boosts helicopter because its PNG has empty padding; we crop to content
# before resize, so that boost only upscales a 32² mask into blocky pixels.
CATEGORY_SCALE_X100 = {
    "drone": 100,
    "military-drone": 50,
    "balloon": 100,
    "airship": 50,
    "glider": 150,
}

DEFAULT_CATEGORY = "large-jet-2"


def cat_ident(category: str) -> str:
    """C++ identifier for a category key (large-jet-2 → large_jet_2)."""
    return category.replace("-", "_")


def alpha_sym(category: str) -> str:
    return f"kIcon_{cat_ident(category)}_alpha"


def coverage_alpha(rgba: Image.Image) -> Image.Image:
    """8-bit coverage from alpha.

    Prefer the alpha channel whenever the image actually uses transparency.
    Only fall back to inverted luminance for fully-opaque black-on-canvas art
    (no transparent pixels) — otherwise gray RGB under alpha=0 falsely floods
    the mask (common in exported Pi silhouettes).
    """
    out = Image.new("L", rgba.size, 0)
    px = out.load()
    src = rgba.load()
    w, h = rgba.size

    has_transparency = False
    for y in range(0, h, max(1, h // 64)):
        for x in range(0, w, max(1, w // 64)):
            if src[x, y][3] < ALPHA_FLOOR:
                has_transparency = True
                break
        if has_transparency:
            break

    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if has_transparency:
                px[x, y] = a if a >= ALPHA_FLOOR else 0
                continue
            if a >= ALPHA_FLOOR:
                px[x, y] = a
            elif (r + g + b) < 40:
                px[x, y] = 0
            else:
                lum = (r + g + b) // 3
                px[x, y] = 255 - lum if lum < 220 else 0
    return out


def paste_scaled_into_square(coverage: Image.Image, side: int, margin_frac: float = 0.06) -> bytes:
    """Fit coverage into a side×side mask, preserving aspect, with margin."""
    bbox = coverage.point(lambda v: 255 if v >= ALPHA_FLOOR else 0).getbbox()
    if bbox is None:
        return bytes(side * side)
    coverage = coverage.crop(bbox)
    w, h = coverage.size
    if w <= 0 or h <= 0:
        return bytes(side * side)

    inner = max(1, int(round(side * (1.0 - 2.0 * margin_frac))))
    scale = min(inner / float(w), inner / float(h))
    nw = max(1, int(round(w * scale)))
    nh = max(1, int(round(h * scale)))
    scaled = coverage.resize((nw, nh), Image.Resampling.LANCZOS)

    canvas = Image.new("L", (side, side), 0)
    canvas.paste(scaled, ((side - nw) // 2, (side - nh) // 2))

    out = bytearray(side * side)
    px = canvas.load()
    for y in range(side):
        for x in range(side):
            out[y * side + x] = px[x, y]
    return bytes(out)


def to_alpha_mask(image: Image.Image, side: int) -> bytes:
    from PIL import ImageFilter

    rgba = image.convert("RGBA")
    coverage = coverage_alpha(rgba)
    bbox = coverage.point(lambda v: 255 if v >= ALPHA_FLOOR else 0).getbbox()
    if bbox is not None:
        coverage = coverage.crop(bbox)

    w, h = coverage.size
    if w <= 0 or h <= 0:
        return bytes(side * side)

    # Normalize to a mid-resolution working size so dilate is independent of
    # 241 vs 1024 source art, then thicken thin strokes by a few working pixels.
    work_max = 96
    work_scale = min(1.0, work_max / float(max(w, h)))
    ww = max(1, int(round(w * work_scale)))
    wh = max(1, int(round(h * work_scale)))
    work = coverage.resize((ww, wh), Image.Resampling.LANCZOS)

    solid = sum(1 for v in work.get_flattened_data() if v >= ALPHA_FLOOR)
    fill = solid / float(ww * wh)
    # Keep silhouettes recognizable: thicken wireframes, not flood-fill them.
    if fill < 0.12:
        dilate = 5
    elif fill < 0.25:
        dilate = 3
    else:
        dilate = 0
    if dilate >= 3:
        work = work.filter(ImageFilter.MaxFilter(dilate))

    return paste_scaled_into_square(work, side)


def emit_u8_array(name: str, data: bytes) -> str:
    lines: list[str] = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        parts = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {parts},")
    body = "\n".join(lines)
    return f"static const uint8_t {name}[] PROGMEM = {{\n{body}\n}};\n"


def normalize_type(code: str) -> str:
    return "".join(str(code).upper().split())


def main() -> int:
    src_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT

    mapping_path = src_dir / "aircraft-icons.json"
    if not mapping_path.is_file():
        print(f"Missing mapping: {mapping_path}")
        return 1

    with mapping_path.open(encoding="utf-8") as fh:
        mapping = json.load(fh)

    icons_meta = mapping.get("icons") or {}
    type_map = mapping.get("typeCodeMapping") or {}

    masks: dict[str, bytes] = {}
    missing: list[str] = []
    for category in CATEGORY_ORDER:
        info = icons_meta.get(category) or {}
        filename = info.get("file") or f"{category}.png"
        png = src_dir / filename
        if not png.is_file() or png.stat().st_size == 0:
            missing.append(category)
            continue
        image = Image.open(png)
        masks[category] = to_alpha_mask(image, ICON_SIDE)
        print(f"  {category}: {png.name} -> {ICON_SIDE}x{ICON_SIDE} alpha")

    if DEFAULT_CATEGORY not in masks:
        print(f"Default category '{DEFAULT_CATEGORY}' mask missing")
        return 1
    if missing:
        print(f"WARNING: missing PNGs for: {', '.join(missing)}")

    # ICAO type → category id (first wins; heli categories kept over later dupes).
    type_to_cat: dict[str, str] = {}
    for category, codes in type_map.items():
        if category.startswith("_") or category not in masks:
            continue
        for code in codes:
            key = normalize_type(code)
            if not key:
                continue
            existing = type_to_cat.get(key)
            if existing in ("helicopter", "military-helicopter"):
                continue
            type_to_cat[key] = category

    type_entries = sorted(type_to_cat.items(), key=lambda kv: kv[0])
    cat_index = {c: i for i, c in enumerate(CATEGORY_ORDER) if c in masks}
    # Remap to dense IDs of present icons only.
    present = [c for c in CATEGORY_ORDER if c in masks]
    cat_index = {c: i for i, c in enumerate(present)}

    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("// Auto-generated by tools/aircraft_icons_to_header.py — do not edit.")
    lines.append("#include <cstdint>")
    lines.append("#include <pgmspace.h>")
    lines.append("")
    lines.append("namespace data::aircraft_icons {")
    lines.append("")
    lines.append(f"constexpr int kIconSide = {ICON_SIDE};")
    lines.append(f"constexpr uint8_t kAlphaFloor = {ALPHA_FLOOR};")
    lines.append(f"constexpr size_t kCategoryCount = {len(present)};")
    lines.append(f"constexpr size_t kTypeMapCount = {len(type_entries)};")
    lines.append(
        f"constexpr uint8_t kDefaultCategory = {cat_index[DEFAULT_CATEGORY]};"
    )
    lines.append("")
    lines.append("enum class Category : uint8_t {")
    for c in present:
        lines.append(f"  {cat_ident(c)} = {cat_index[c]},")
    lines.append("};")
    lines.append("")

    for c in present:
        lines.append(emit_u8_array(alpha_sym(c), masks[c]))
        lines.append("")

    lines.append("struct IconBlob {")
    lines.append("  const uint8_t* alpha;  // PROGMEM, kIconSide*kIconSide")
    lines.append("  uint16_t scale_x100;   // draw size multiplier")
    lines.append("};")
    lines.append("")
    lines.append("static const IconBlob kIcons[] PROGMEM = {")
    for c in present:
        scale = CATEGORY_SCALE_X100.get(c, 100)
        lines.append(f"  {{{alpha_sym(c)}, {scale}}},")
    lines.append("};")
    lines.append("")
    lines.append("struct TypeEntry {")
    lines.append("  char type[5];")
    lines.append("  uint8_t category;")
    lines.append("};")
    lines.append("")
    lines.append("static const TypeEntry kTypeMap[] PROGMEM = {")
    for code, category in type_entries:
        if category not in cat_index:
            continue
        padded = code[:4].ljust(4)
        chars = ", ".join(f"'{c}'" for c in padded) + ", '\\0'"
        lines.append(f"  {{{{{chars}}}, {cat_index[category]}}},")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace data::aircraft_icons")
    lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    alpha_bytes = len(present) * ICON_SIDE * ICON_SIDE
    print(
        f"Wrote {out_path} — {len(present)} icons, {len(type_entries)} types, "
        f"~{alpha_bytes} B alpha"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
