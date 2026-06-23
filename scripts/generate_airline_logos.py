# PlatformIO pre-build: refresh airline logo lookup from PNG assets.
Import("env")

import subprocess
import sys
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"])
tool = project_dir / "tools" / "airline_logos_to_header.py"
src_dir = project_dir / "data" / "airline_logos"
out_header = project_dir / "include" / "data" / "airline_logos_lookup.h"

if not src_dir.is_dir() or not any(src_dir.glob("*.png")):
    if out_header.is_file():
        print("Airline logos: using existing generated header.")
    else:
        print("Airline logos: no PNG source and no header; logos disabled.")
else:
    print("Refreshing airline logo lookup...")
    result = subprocess.run(
        [sys.executable, str(tool), str(src_dir), str(out_header)],
        cwd=str(project_dir),
    )

    if result.returncode != 0:
        print(
            "WARNING: airline logo refresh failed; "
            "continuing with existing generated files."
        )
