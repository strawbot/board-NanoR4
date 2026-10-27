#!/usr/bin/env python3
"""
gen_robot_page.py — PlatformIO pre-build hook for env:nano_r4_web

Turns ../Robot/net/web/robot_page.html into Board/web/robot_page.c (the page
as a ready-to-send HTTP response) with the board-agnostic generator that
lives next to the page. The output is rewritten only when the page changed.
Board/web/ is excluded from the other environments' source lists in
platformio.ini and git-ignored.
"""
import os
import subprocess
import sys

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

project = env.subst("$PROJECT_DIR")  # type: ignore[name-defined]
web = os.path.join(project, "..", "Robot", "net", "web")
out_dir = os.path.join(project, "Board", "web")
os.makedirs(out_dir, exist_ok=True)

subprocess.check_call([
    sys.executable,
    os.path.join(web, "gen_page.py"),
    os.path.join(web, "robot_page.html"),
    os.path.join(out_dir, "robot_page.c"),
])
