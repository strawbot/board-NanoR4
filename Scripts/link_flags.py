#!/usr/bin/env python3
"""
link_flags.py — PlatformIO pre-build hook

Adds -nostartfiles to the link. FSP's Reset_Handler is the entry point, so the
C runtime's crt0 (its _start, and the exit/atexit/stdio cleanup that _start
drags in) is never used; FSP's own generated projects link with -nostartfiles
for the same reason. build_flags does not reach the link step in this
project, hence a script.
"""
Import("env")  # noqa: F821 — injected by PlatformIO/SCons

env.Append(LINKFLAGS=["-nostartfiles"])  # type: ignore[name-defined]
