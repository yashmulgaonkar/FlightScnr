#!/usr/bin/env python3
"""Convert a TrueType font to a 4-bit alpha AaFont header (7-bit ASCII)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import freetype
except ImportError:
    print("Install freetype-py: pip install freetype-py", file=sys.stderr)
    raise

DPI = 141  # Match Adafruit / ttf_to_gfxfont.py so pt sizes stay the same


def sanitize_name(path: Path, size: int) -> str:
    stem = re.sub(r"[^0-9A-Za-z]", "", path.stem)
    return f"{stem}{size}pt4aa"


def convert(ttf: Path, size: int, first: int = ord(" "), last: int = ord("~")) -> str:
    face = freetype.Face(str(ttf))
    face.set_char_size(size * 64, 0, DPI, 0)

    font_name = sanitize_name(ttf, size)
    glyphs: list[dict] = []
    bitmap_bytes: list[int] = []
    bitmap_offset = 0

    for code in range(first, last + 1):
        face.load_char(code, freetype.FT_LOAD_RENDER)
        bitmap = face.glyph.bitmap
        left = face.glyph.bitmap_left
        top = face.glyph.bitmap_top
        advance = face.glyph.advance.x >> 6
        width = bitmap.width
        height = bitmap.rows
        pitch = bitmap.pitch
        buf = bitmap.buffer

        glyphs.append(
            {
                "bitmapOffset": bitmap_offset,
                "width": width,
                "height": height,
                "xAdvance": advance,
                "xOffset": left,
                "yOffset": 1 - top,
            }
        )

        packed: list[int] = []
        nibble_hi = True
        acc = 0
        for y in range(height):
            for x in range(width):
                idx = y * pitch + x
                gray = buf[idx] if isinstance(buf[idx], int) else ord(buf[idx])
                a4 = min(15, gray >> 4)
                if nibble_hi:
                    acc = a4 << 4
                    nibble_hi = False
                else:
                    packed.append(acc | a4)
                    nibble_hi = True
                    acc = 0
        if not nibble_hi:
            packed.append(acc)

        bitmap_bytes.extend(packed)
        bitmap_offset += len(packed)

    lines = [
        "#pragma once",
        "",
        '#include "hardware/aa_font.h"',
        "",
        f"const uint8_t {font_name}Bitmaps[] PROGMEM = {{",
    ]

    row: list[str] = []
    for i, b in enumerate(bitmap_bytes):
        row.append(f"0x{b:02X}")
        if len(row) == 12:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row) + ",")
    if bitmap_bytes:
        lines[-1] = lines[-1].rstrip(",")
    lines.append("};")
    lines.append("")
    lines.append(f"const AaGlyph {font_name}Glyphs[] PROGMEM = {{")

    for i, g in enumerate(glyphs):
        code = first + i
        comma = "," if code < last else ""
        suffix = f"  // 0x{code:02X}"
        if 32 <= code <= 126:
            suffix += f" '{chr(code)}'"
        lines.append(
            f"  {{ {g['bitmapOffset']:5d}, {g['width']:3d}, {g['height']:3d}, "
            f"{g['xAdvance']:3d}, {g['xOffset']:4d}, {g['yOffset']:4d} }}"
            f"{comma}{suffix}"
        )

    lines.append("};")
    y_advance = face.size.height >> 6
    if y_advance == 0 and glyphs:
        y_advance = max(1, glyphs[0]["height"])

    lines += [
        "",
        f"const AaFont {font_name} PROGMEM = {{",
        f"  {font_name}Bitmaps,",
        f"  {font_name}Glyphs,",
        f"  0x{first:02X}, 0x{last:02X}, {y_advance}",
        "};",
        "",
        f"// Approx. {bitmap_offset + (last - first + 1) * 10 + 8} bytes",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="TTF to 4-bit AaFont header")
    parser.add_argument("ttf", type=Path)
    parser.add_argument("size", type=int, help="Font size in points")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()

    header = convert(args.ttf, args.size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf-8")
    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
