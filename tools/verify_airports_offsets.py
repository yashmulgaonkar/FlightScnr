#!/usr/bin/env python3
"""Verify ICAO -> city offsets in generated airports_lookup.cpp (byte pool)."""
from __future__ import annotations

import re
import sys
from pathlib import Path

CHAR4 = re.compile(
    r"\{\s*'(.?)'\s*,\s*'(.?)'\s*,\s*'(.?)'\s*,\s*'(.?)'\s*\}\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)"
)
IATA_ROW = re.compile(
    r"\{\s*\{\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*\}\s*,\s*"
    r"\{\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*,\s*'((?:\\0|[^'])*)'\s*\}\s*\}"
)


def iata_chars(groups: tuple[str, ...]) -> str:
    return "".join("" if g == "\\0" else g for g in groups)
BYTE = re.compile(r"0x[0-9a-fA-F]{2}")


def parse_names_pool(cpp: str) -> bytes:
    start = cpp.index("const char kNames[] PROGMEM = {")
    end = cpp.index("};", start)
    blob = cpp[start:end]
    return bytes(int(h, 16) for h in BYTE.findall(blob))


def pool_string_at(pool: bytes, offset: int) -> str:
    end = pool.find(b"\0", offset)
    if end < 0:
        end = len(pool)
    return pool[offset:end].decode("utf-8", errors="replace")


def main() -> int:
    cpp_path = Path(__file__).resolve().parent.parent / "src" / "data" / "airports_lookup.cpp"
    cpp = cpp_path.read_text(encoding="utf-8")
    pool = parse_names_pool(cpp)

    entries: dict[str, tuple[int, str]] = {}
    for m in CHAR4.finditer(cpp):
        chars = [m.group(i) for i in range(1, 5)]
        if not all(c.isalnum() for c in chars):
            continue
        icao = "".join(chars)
        off = int(m.group(5))
        if off >= len(pool):
            continue
        entries[icao] = (off, pool_string_at(pool, off))

    print(f"pool bytes {len(pool)}  icao entries {len(entries)}")

    expected = {
        "KSFO": "San Francisco",
        "RJAA": "Tokyo",
        "KBOS": "Boston",
        "ZSPD": "Shanghai",
    }
    ok = True
    for icao, want in expected.items():
        if icao not in entries:
            print(f"{icao}: missing")
            ok = False
            continue
        off, got = entries[icao]
        mark = "OK" if got == want else "MISMATCH"
        if got != want:
            ok = False
        print(f"{icao} @{off}: {got!r} ({mark})")

    for iata in ("SFO", "NRT", "PVG", "BOS"):
        for m in IATA_ROW.finditer(cpp):
            key = iata_chars(tuple(m.group(i) for i in range(1, 4)))
            if key != iata:
                continue
            icao = iata_chars(tuple(m.group(i) for i in range(5, 9)))
            city = entries.get(icao, (0, "?"))[1]
            print(f"IATA {iata} -> {icao} ({city!r})")
            break
        else:
            print(f"IATA {iata}: missing")
            ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
