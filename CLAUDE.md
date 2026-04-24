# Nano/ — Binding rules for Claude

This folder is the Nano board project (Renesas RA4 / FSP / PlatformIO).
Authoritative project-wide rules live at the top-level `CLAUDE.md` and at
`Robot/CLAUDE.md`. Read those first; the rules below are the Nano-specific
reinforcements.

## Sibling-ignorance

Nano must not reach into any other board folder. Do not include files from,
reference paths inside, or copy-paste code out of `Discovery/`,
`Nucleo411/`, `Nucleo446/`, `PNucleo/`, or `TIVA/`. If another board has a
feature Nano also wants, the answer is to factor that feature up into
`Robot/` and then compose it here — never cross-link.

## Compose from Robot, don't duplicate it

Features shared across boards live in `Robot/`. Nano picks them up by:

1. Adding the relevant `Robot/<area>/<feature>/` sources to Nano's build
   (CMake / `platformio.ini`).
2. Calling `feature_init()` (and any registration hooks) from Nano's `main`.
3. Supplying any hardware shim the feature asks for via its
   `feature_bind_hw()` entry point — the shim lives **here**, in Nano.

If a Robot module needs behavior only Nano can provide (FSP peripheral,
pinout, linker symbol), implement it in a Nano file that satisfies the hook,
not by editing the Robot module.

## What belongs in Nano/

- FSP-generated code (`ra/`, `ra_gen/`, `ra_cfg/`) and `configuration.xml`.
- Linker scripts, `memory_regions*.ld`, `bsp_linker_info.h`.
- Board-specific drivers (anything that includes `hal_data.h`, `r_*.h`, or
  touches Renesas peripherals).
- Nano-only CLI words (commands that read Renesas registers, dump FSP
  state, etc.). Register them with `cli_register(...)` from a Nano init
  function.
- The Nano `main` that wires Robot features together.

## What does NOT belong in Nano/

- Board-agnostic protocol code or diagnostics that would work unchanged on
  another board. Those belong in `Robot/`.
- Anything that another board would need to copy to use.

## When adding a feature

If the feature could plausibly run on a Discovery/Nucleo/PNucleo/TIVA board
with only hardware glue swapped out, factor it into `Robot/` first, then
adopt it here. Do not implement it as a Nano-local module that later has to
be generalized.
