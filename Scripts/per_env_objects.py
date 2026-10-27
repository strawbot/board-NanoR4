#!/usr/bin/env python3
"""
per_env_objects.py — PlatformIO pre-build hook

PlatformIO builds sources that live outside the project folder (../tinyusb,
../Robot, ../../TimbreOS) into .pio/build/<folder name>/, shared by every
environment, yet each environment tracks those objects in its own database.
So after `pio run -e nano_r4_web` the next `pio run -e nano_r4` believes the
objects are current and links TinyUSB compiled with the NCM class in, and
likewise nano_r4_double's FLOAT_DOUBLE TimbreOS objects.

Fix: remember which environment last built, and when it changes delete the
shared object folders so they are rebuilt with the new environment's flags.
Switching environments costs a rebuild of those folders; staying in one does
not.
"""
import os
import shutil

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

SHARED = ("tinyusb", "Robot", "TimbreOS")

build_root = env.subst("$PROJECT_BUILD_DIR")  # type: ignore[name-defined]
marker = os.path.join(build_root, ".shared_objects_env")
this_env = env["PIOENV"]  # type: ignore[name-defined]

try:
    with open(marker) as f:
        last_env = f.read().strip()
except OSError:
    last_env = ""

if last_env != this_env:
    for name in SHARED:
        shutil.rmtree(os.path.join(build_root, name), ignore_errors=True)
    os.makedirs(build_root, exist_ok=True)
    with open(marker, "w") as f:
        f.write(this_env)
