#!/usr/bin/env python3
"""
report_memory.py — PlatformIO post-build hook

Replaces PlatformIO's built-in CheckUploadSize (which shows 0% because it uses
board-JSON memory sizes that don't match our bootloader-offset linker script)
with a correct report derived from the actual ELF sections.

Flash available = FLASH_GAP (0x400) + FLASH (0x3BC00) = 0x3C000 = 245,760 B
RAM available   = RAM (0x8000) = 32,768 B
"""
import subprocess
import sys

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

FLASH_TOTAL = 0x3C000   # 240 KB (above bootloader)
RAM_TOTAL   = 0x8000    # 32 KB

def report_memory(env, source=None, target=None):
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

# AddMethod overwrites the existing CheckUploadSize method so PlatformIO's
# VerboseAction wrapper calls ours instead (env.Replace writes to the wrong dict).
env.AddMethod(report_memory, "CheckUploadSize")
