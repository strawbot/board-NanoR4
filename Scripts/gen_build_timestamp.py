#!/usr/bin/env python3
"""
gen_build_timestamp.py — PlatformIO pre-build hook

Delegates to TimbreOS/gen_build_timestamp.py, writing src/build_timestamp.c
so that `build_timestamp[]` reflects the actual build time rather than the
last edit time of clocks.c.
"""
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

PROJECT_DIR = Path(env["PROJECT_DIR"])  # type: ignore[name-defined]
SCRIPT  = PROJECT_DIR.parent / "TimbreOS" / "gen_build_timestamp.py"
OUT     = PROJECT_DIR / "src" / "build_timestamp.c"

result = subprocess.run([sys.executable, str(SCRIPT), str(OUT)])
if result.returncode != 0:
    sys.stderr.write("[gen_build_timestamp] ERROR: script exited %d\n" % result.returncode)
    sys.exit(1)
