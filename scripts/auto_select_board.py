"""
Compile-time board auto-selection for FlightScnr.

Resolution order (for env `auto` only):
  1. FLIGHTSCNR_BOARD environment variable
  2. USB serial heuristics (CH343 companion → Waveshare)
  3. Last successful selection in .pio/last_board
  4. Default: tencoder-pro

Explicit envs (`tencoder-pro`, `waveshare-knob-1.8`) keep their platformio.ini macros.
"""

from __future__ import annotations

import os
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # noqa: F821 — PlatformIO SCons

PROJECT_DIR = Path(env["PROJECT_DIR"])
PIO_DIR = PROJECT_DIR / ".pio"
LAST_BOARD_PATH = PIO_DIR / "last_board"

BOARD_TENCODER = "tencoder-pro"
BOARD_WAVESHARE = "waveshare-knob-1.8"
VALID_BOARDS = {BOARD_TENCODER, BOARD_WAVESHARE}

BOARD_MACROS = {
    BOARD_TENCODER: "FLIGHTSCNR_BOARD_TENCODER_PRO",
    BOARD_WAVESHARE: "FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18",
}


def _normalize(name: str | None) -> str | None:
    if not name:
        return None
    name = name.strip().lower().replace("_", "-")
    aliases = {
        "tencoder": BOARD_TENCODER,
        "tencoder-pro": BOARD_TENCODER,
        "lilygo": BOARD_TENCODER,
        "waveshare": BOARD_WAVESHARE,
        "waveshare-knob": BOARD_WAVESHARE,
        "waveshare-knob-1.8": BOARD_WAVESHARE,
        "knob-1.8": BOARD_WAVESHARE,
    }
    resolved = aliases.get(name, name)
    return resolved if resolved in VALID_BOARDS else None


def _read_last_board() -> str | None:
    try:
        return _normalize(LAST_BOARD_PATH.read_text(encoding="utf-8"))
    except OSError:
        return None


def _write_last_board(board: str) -> None:
    try:
        PIO_DIR.mkdir(parents=True, exist_ok=True)
        LAST_BOARD_PATH.write_text(board + "\n", encoding="utf-8")
    except OSError:
        pass


def _detect_usb_board() -> str | None:
    """Best-effort USB probe. Waveshare's CH334 hub exposes a CH343 UART bridge."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None

    waveshare_hints = 0
    for port in list_ports.comports():
        blob = " ".join(
            filter(
                None,
                [
                    getattr(port, "description", None),
                    getattr(port, "manufacturer", None),
                    getattr(port, "product", None),
                    getattr(port, "hwid", None),
                ],
            )
        ).upper()
        if "1A86" in blob or "CH343" in blob or "CH340" in blob:
            waveshare_hints += 1
        if "WAVESHARE" in blob or "KNOB-TOUCH" in blob:
            waveshare_hints += 2

    if waveshare_hints > 0:
        return BOARD_WAVESHARE
    return None


def _resolve_auto_board() -> str:
    forced = _normalize(os.environ.get("FLIGHTSCNR_BOARD"))
    if forced:
        print(f"[board] FLIGHTSCNR_BOARD → {forced}")
        return forced

    detected = _detect_usb_board()
    if detected:
        print(f"[board] USB heuristic → {detected}")
        return detected

    last = _read_last_board()
    if last:
        print(f"[board] last selection → {last}")
        return last

    print(f"[board] default → {BOARD_TENCODER}")
    return BOARD_TENCODER


def _apply_board_flags(board: str) -> None:
    macro = BOARD_MACROS[board]
    flags = env.get("BUILD_FLAGS", [])
    cleaned = []
    for flag in flags:
        if "FLIGHTSCNR_BOARD_" in str(flag):
            continue
        cleaned.append(flag)
    cleaned.append(f"-D {macro}")
    env.Replace(BUILD_FLAGS=cleaned)


pioenv = env["PIOENV"]
if pioenv == "auto":
    board = _resolve_auto_board()
    _write_last_board(board)
    _apply_board_flags(board)
    print(f"[board] auto → {board} ({BOARD_MACROS[board]})")
elif pioenv in VALID_BOARDS:
    _write_last_board(pioenv)
    print(f"[board] env {pioenv} ({BOARD_MACROS[pioenv]})")
else:
    print(f"[board] env {pioenv} (no board remap)")
