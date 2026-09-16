#!/usr/bin/env python3
"""
resolve_action.py

mstats and te print an unnamed action as a bare hex address (see
tea.c:machineStats()) - useful for noticing something is unusual, useless
for knowing what to do about it without a .map file search. This resolves
that address straight to a function name and source line via addr2line,
so the next step - deciding whether it's worth a namedAction() call - is
a one-line lookup instead.

namedAction() costs flash for every name whether or not that action ever
turns out to be interesting, so it's meant to stay opt-in: add it only for
actions actually worth watching, and only at startup (see init() in
cli.c, alongside the existing namedAction() calls). It just associates a
string with a function pointer for the stats dictionary - calling it
later doesn't rename entries the target already recorded under the bare
pointer; that needs a reset (0stats) or a reboot to take effect.

Usage:
    python3 Scripts/resolve_action.py BA40 7AF0
    python3 Scripts/resolve_action.py 0xBA40
    python3 Scripts/resolve_action.py --elf .pio/build/nano_r4/firmware.elf BA40
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

DEFAULT_ELF = pathlib.Path(__file__).resolve().parent.parent / ".pio" / "build" / "nano_r4" / "firmware.elf"


def find_addr2line(override: str | None) -> str:
    if override:
        return override
    path = shutil.which("arm-none-eabi-addr2line")
    if path:
        return path
    sys.exit(
        "error: arm-none-eabi-addr2line not found on PATH.\n"
        "Install the ARM GNU toolchain, or pass --addr2line /path/to/it."
    )


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "addresses", nargs="+",
        help="hex addresses as printed by mstats/te, with or without 0x",
    )
    ap.add_argument("--elf", type=pathlib.Path, default=DEFAULT_ELF,
                     help=f"default: {DEFAULT_ELF}")
    ap.add_argument("--addr2line", default=None, help="override the addr2line binary")
    args = ap.parse_args()

    if not args.elf.exists():
        sys.exit(f"error: {args.elf} does not exist. Build the firmware first, or pass --elf.")

    tool = find_addr2line(args.addr2line)
    normalized = [a if a.lower().startswith("0x") else f"0x{a}" for a in args.addresses]

    result = subprocess.run(
        [tool, "-f", "-C", "-e", str(args.elf), *normalized],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.exit(f"error: {tool} failed:\n{result.stderr}")

    # addr2line -f prints two lines per address: the function name, then
    # file:line. Zipped back up against the addresses we sent, in order.
    lines = result.stdout.splitlines()
    pairs = list(zip(lines[0::2], lines[1::2]))

    for addr, (name, loc) in zip(normalized, pairs):
        print(f"{addr}  ->  {name}   {loc}")
        if name not in ("??", "??:0"):
            print(f"          namedAction({name});   "
                  f"// add at startup in init(), alongside the others")
        print()


if __name__ == "__main__":
    main()
