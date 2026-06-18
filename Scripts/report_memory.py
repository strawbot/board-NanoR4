#!/usr/bin/env python3
"""
report_memory.py — PlatformIO post-build hook

Replaces PlatformIO's built-in CheckUploadSize (which shows 0% because its
SIZECHECKCMD doesn't work with our pinned toolchain) with a correct report
derived from the ELF sections.

Why .method mutation instead of env.AddMethod():
  PlatformIO's VerboseAction captures a reference to the MethodWrapper object
  at configure time (env.VerboseAction(env.CheckUploadSize, ...)).  Calling
  env.AddMethod() later creates a NEW MethodWrapper that the already-captured
  reference never sees.  Mutating wrapper.method in place redirects the
  captured reference to our function.

Flash available = FLASH_GAP (0x400) + FLASH (0x3BC00) = 0x3C000 = 245,760 B
RAM available   = RAM (0x8000) = 32,768 B
"""
import subprocess
import sys

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

FLASH_TOTAL = 0x3C000   # 240 KB (above bootloader)
RAM_TOTAL   = 0x8000    # 32 KB


def report_memory(bound_env, **kwargs):
    # bound_env is the MethodWrapper's captured env object.
    # SCons also passes env= as a keyword arg; **kwargs absorbs it safely.
    env = bound_env
    elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    size_tool = env.subst("$SIZETOOL") or env.subst("$CC").replace("gcc", "size")

    try:
        result = subprocess.run(
            [size_tool, "--format=sysv", elf],
            capture_output=True, text=True
        )
    except FileNotFoundError:
        result = subprocess.run(
            ["arm-none-eabi-size", "--format=sysv", elf],
            capture_output=True, text=True
        )

    if result.returncode != 0:
        sys.stderr.write("[report_memory] size failed: %s\n" % result.stderr)
        return

    flash_used = 0
    ram_used   = 0
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            size = int(parts[1])
            addr = int(parts[2])
        except ValueError:
            continue
        if 0x00000000 <= addr < 0x20000000:
            flash_used += size
        elif addr >= 0x20000000:
            ram_used += size

    flash_pct = 100.0 * flash_used / FLASH_TOTAL
    ram_pct   = 100.0 * ram_used   / RAM_TOTAL

    print("  Flash: {:7,} / {:,} bytes  ({:.1f}%)".format(flash_used, FLASH_TOTAL, flash_pct))
    print("  RAM:   {:7,} / {:,} bytes  ({:.1f}%)".format(ram_used,   RAM_TOTAL,   ram_pct))


# Mutate the existing MethodWrapper in place — the VerboseAction holds a
# reference to this same object, so it will call our function instead.
wrapper = env.__dict__.get("CheckUploadSize")
if wrapper is not None and hasattr(wrapper, "method"):
    wrapper.method = report_memory
else:
    env.AddMethod(report_memory, "CheckUploadSize")
