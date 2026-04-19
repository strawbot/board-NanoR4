#!/usr/bin/env python3
"""
run_parsewords.py — PlatformIO pre-build hook
─────────────────────────────────────────────
Invokes ../TimbreOS/WordLists/parsewords.py with Board/boardwords.txt
to regenerate wordlist.c, help.c, and wordlist.txt as needed.

parsewords.py does its own mtime-based dependency check (covers the
input file plus every file it Includes, transitively, plus parsewords.py
itself). This wrapper just runs it on every build — parsewords.py
no-ops silently when nothing has changed.

Wired in via platformio.ini:
    extra_scripts = pre:Scripts/run_parsewords.py
"""
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

PROJECT_DIR = Path(env["PROJECT_DIR"])  # type: ignore[name-defined]
PARSEWORDS  = PROJECT_DIR.parent / "TimbreOS" / "WordLists" / "parsewords.py"
BINDINGS    = PROJECT_DIR / "Board" / "boardwords.txt"


def fail(msg):
    sys.stderr.write("[run_parsewords] ERROR: %s\n" % msg)
    sys.exit(1)


if not PARSEWORDS.is_file():
    fail("parsewords.py not found at %s" % PARSEWORDS)
if not BINDINGS.is_file():
    fail("boardwords.txt not found at %s" % BINDINGS)

print("[run_parsewords] checking %s" % BINDINGS.relative_to(PROJECT_DIR))
result = subprocess.run(
    [sys.executable, str(PARSEWORDS), str(BINDINGS)],
    cwd=str(PROJECT_DIR),
)
if result.returncode != 0:
    fail("parsewords.py exited with code %d" % result.returncode)
