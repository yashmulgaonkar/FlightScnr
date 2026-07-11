# PlatformIO pre-build: refresh aircraft icon alpha masks from Pi-style PNGs.
Import("env")

import subprocess
import sys
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"])
tool = project_dir / "tools" / "aircraft_icons_to_header.py"
src_candidates = [
    project_dir / "assets" / "aircraft_icons",
    project_dir / "tools" / ".cache" / "aircraft_icons",
]
out_header = project_dir / "include" / "data" / "aircraft_icons_lookup.h"


def png_count(folder: Path) -> int:
    if not folder.is_dir():
        return 0
    return sum(1 for p in folder.glob("*.png") if p.is_file() and p.stat().st_size > 0)


src_dir = max(src_candidates, key=png_count)
if png_count(src_dir) == 0:
    if out_header.is_file():
        print("Aircraft icons: using existing generated header.")
    else:
        print("Aircraft icons: no PNG source and no header; vector fallback only.")
else:
    print(f"Refreshing aircraft icon lookup from {src_dir}...")
    result = subprocess.run(
        [sys.executable, str(tool), str(src_dir), str(out_header)],
        cwd=str(project_dir),
    )
    if result.returncode != 0:
        print(
            "WARNING: aircraft icon refresh failed; "
            "continuing with existing generated files."
        )
