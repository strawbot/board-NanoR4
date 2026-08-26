#!/usr/bin/env python3
"""
kill_stale_debuggers.py — PlatformIO pre-build hook

A debug backend left running from a previous session (e.g. the VS Code
session was closed, or its Stop/Restart controls stopped responding,
without the process actually terminating) holds the probe's USB connection
and/or GDB port open, so the next debug launch can't cleanly acquire
either -- symptoms range from needing to physically unplug/replug a probe,
to a debug session that connects but whose Stop/Restart controls do
nothing, to VS Code's debug panel disappearing on a window reload while
the backend keeps running invisibly. None of this was ever a problem with
PlatformIO's own debug config (monitor reset/halt) or our custom
openocd.cfg -- it was always leftover processes.

Covers both debug tool generations used on this board this session:
JLinkGDBServer (Segger's own server, used with the J-Link probe) and
openocd/arm-none-eabi-gdb (used with the ST-Link probe via our hand-rolled
openocd.cfg, since RA4M1 has no official OpenOCD target script).

Killing any of these before a build is harmless even outside a debug
session -- nothing else depends on them being alive -- so this runs
unconditionally as a pre: script rather than only gating on the debug
target.
"""
import subprocess

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

for name in ("JLinkGDBServer", "JLinkGUIServerExe", "openocd", "arm-none-eabi-gdb"):
    subprocess.run(["pkill", "-f", name], check=False)
