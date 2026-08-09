# Nano R4 reset diagnosis — bootloader disassembled, hang located

Your dump has been reconstructed to a binary (`Docs/nano_r4_bootloader_16k.bin`,
16 KB, code ends at `0x313F`) and disassembled. The bootloader matches
[arduino/arduino-renesas-bootloader](https://github.com/arduino/arduino-renesas-bootloader/blob/main/src/main.c)
instruction for instruction, so the source is a reliable guide — and the
disassembly identifies a specific unbounded wait that accounts for the C board's
symptom exactly.

CLI notes: comments are `( ... )`; `dump ( a n - )` takes n *lines* of 16 bytes.

---

## The hang: an unbounded spin in the bootloader's Reset_Handler

At `0x1EDC`, before clocks, before the LED, before USB, before `main()`:

```
001eee  movw   r3, #0xa502
001ef2  strh.w r3, [r0, #0x3fe]   ( R_SYSTEM->PRCR = 0xA502, unlock PRC1 )
001ef6  movs   r3, #1
001ef8  strb.w r3, [r0, #0x41f]   ( R_SYSTEM->VBTCR1 = 1, BPWSWSTP )
001efc  ldrb.w r3, [r0, #0x4b1]   ( R_SYSTEM->VBTSR )
001f00  lsls   r7, r3, #0x1b      ( shift bit 4 — VBTRVLD — into the sign bit )
001f02  bpl    #0x1efc            ( ***spin until VBTRVLD == 1, no timeout*** )
```

That is FSP's `SystemInit`, verbatim from your own tree
(`ra/fsp/src/bsp/cmsis/Device/RENESAS/Source/system.c`):

```c
R_SYSTEM->PRCR = BSP_PRV_PRCR_PRC1_UNLOCK;
R_SYSTEM->VBTCR1 = 1U;
FSP_HARDWARE_REGISTER_WAIT(R_SYSTEM->VBTSR_b.VBTRVLD, 1U);
```

`FSP_HARDWARE_REGISTER_WAIT` is a bare `while` with **no timeout and no escape**.
The comment above it explains why it's there: "LOCOCR, LOCOUTCR, SOSCCR, and
SOMCR cannot be accessed until VBTSR.VBTRVLD is set."

If the VBATT domain doesn't report valid, the MCU spins here forever. Nothing
has been initialised yet, so the board is completely silent: **no LED, no USB, no
CLI, no fault** — which is precisely what the C board does after a button press.
This is also why no amount of poking at USB, I²C, or the app was ever going to
help; the CPU never gets within a mile of your code.

### Correction: VBTRVLD is one of five unbounded spins, not the only one

`show-time` increments on both boards, so the 32.768 kHz sub-clock runs on both
and the missing-crystal theory is out.

Being precise about what the disassembly actually shows: `Reset_Handler`
contains **five** unbounded waits, any of which could be the hang.

| Address | Spins on | Register |
| --- | --- | --- |
| `0x1EFC`–`0x1F02` | `VBTSR.VBTRVLD` (bit 4) | `0x4001_E4B1` |
| `0x1F48`–`0x1F4E` | `OSCSF.HOCOSF` (bit 0) | `0x4001_E03C` |
| `0x1F52`–`0x1F5C` | `OPCCR.OPCMTSF` (bit 4) | `0x4001_E0A0` |
| `0x1F62`–`0x1F68` | `OPCCR.OPCMTSF` (bit 4) | `0x4001_E0A0` |
| `0x1F72`–`0x1F78` | `OSCSF.HOCOSF` (bit 0) | `0x4001_E03C` |

`VBTRVLD` remains the best candidate because it is the only one that depends on
anything off-die, but it is not proven.

Note also `0x1F3E` (`cbz` on `OPCCR`) and `0x1F44` (`cbnz` on `HOCOCR`): the
path taken through this function **depends on register state at entry**. After a
software reset those registers still hold the application's settings; after a
pin reset they are at their defaults. So `Reset_Handler` genuinely executes
different code depending on reset type, which supports the conclusion that the
divergence lives here regardless of which spin is the one that hangs.

### Why a button reset and not the other two

`VBTRVLD` reflects the battery-backup domain's reset state, and the reset types
differ in how they treat that domain:

- **Power-on reset** — the domain comes up cold with the rail, gets its normal
  power-on sequencing, and validates. Works on both boards.
- **`NVIC_SystemReset`** — doesn't reset the VBATT domain at all. `BPWSWSTP` is
  already 1 and `VBTRVLD` is already 1, so the wait falls straight through.
  Works on both boards.
- **RES pin reset** — the domain has to re-validate while VCC is already up and
  the rail never dropped. The micro board manages it. The C board apparently
  does not.

Line that up against your matrix and every row is accounted for:

| Board | Reset | VBATT domain must re-validate? | Result |
| --- | --- | --- | --- |
| Micro | Power cycle | cold, normal sequencing | works |
| Micro | `reboot` | no | works |
| Micro | Button | yes | works |
| C | Power cycle | cold, normal sequencing | works |
| C | `reboot` | no | works |
| C | Button | **yes** | **hangs, silent** |

This also retires the earlier PORF/`set_double_tap_data` correlation. That
theory needed the hang to be after `main()` was reached; this one puts it before
`SystemInit` even finishes, which fits "no LED" without needing `blink()` to
have been called.

---

## Read the VBATT domain and compare the two boards

This is the test that should settle it. All read-only, from a working CLI on
each board:

```
hex
4001E4B1 c@ .h    ( VBTSR   bit0 VBTRDF bit1 VBTBLDF bit4 VBTRVLD )
4001E41F c@ .h    ( VBTCR1  bit0 BPWSWSTP )
4001E4C0 c@ .h    ( VBTBER  bit3 VBAE — backup register access enable )
4001E480 c@ .h    ( SOSCCR  bit0 SOSTP — sub-clock stopped )
```

What to look for:

- **`VBTSR` bit 1, `VBTBLDF`** — "VBATT Battery Low voltage Detect Flag". If the
  C board sets this and the micro board doesn't, the VBATT supply is low or
  absent, and that is your root cause in one bit.
- **`VBTSR` bit 0, `VBTRDF`** — VBAT_R reset detect. Differences here say the two
  domains are reaching different states.
- **`VBTSR` bit 4, `VBTRVLD`** — should read 1 on both while running, since you
  got past the spin. If either reads 0 while the CLI is alive, something is
  stranger than expected.

Then check the hardware, because a difference in these bits points straight at
it:

1. **Measure the VBATT pin on both boards.** Usually it's tied to 3V3 when
   battery backup isn't used. A backup cap, a diode, or a floating pin on one
   board and not the other would explain everything.
2. **Look for the 32.768 kHz sub-clock crystal** (XCIN/XCOUT) on each board.
   The VBATT domain and the sub-clock share the same corner of the chip, and the
   bootloader stops the sub-clock at `0x1F36` (`SOSCCR = 1`) right after this
   sequence. A missing or non-starting crystal is a plausible reason for the
   domain not validating promptly.

If the boards differ physically here — and given one is USB-C and one is
micro-USB, they are probably not the same design at all — that is the answer,
and it is a hardware difference you can point at rather than a firmware bug.

---

## What the disassembly confirmed about the rest of the boot path

For the record, since it's now verified rather than inferred:

**Double-tap check** (`0x20E2`), the first thing `main()` does:

```
0020e2  ldr    r3, ← 4001E000        ( R_SYSTEM )
0020e4  ldr    r2, ← 07738135        ( DOUBLE_TAP_MAGIC )
0020e6  ldr.w  r1, [r3, #0x500]      ( VBTBKR[0] @ 0x4001E500 )
0020ea  cmp    r1, r2
0020ec  bne.w  #0x23be               ( no match → normal boot )
```

So `0x4001_E500` and `0x0773_8135` were the right target. Your Test B wrote the
correct value to the correct place; it simply did not survive
`NVIC_SystemReset`. Since a real double-tap *does* survive a pin reset on the
micro board, the empirical conclusion is that `VBTBKR` retains across a pin
reset but not across a software reset on these boards.

**PORF-gated arm** (`0x23BE`):

```
0023be  ldrb.w r1, [r3, #0x410]      ( RSTSR0 )
0023c2  lsls   r6, r1, #0x1f         ( bit0 = PORF )
0023c4  bmi    #0x23da               ( PORF set → skip arming )
0023ce  str.w  r2, [r3, #0x500]      ( VBTBKR[0] = magic )
```

**500 ms window** (`0x23DA`) — spins on a millisecond counter at `0x200010C8`:

```
0023dc  ldr    r3, [r2]
0023de  cmp.w  r3, #0x1f4            ( 500 )
0023e2  blo    #0x23dc
```

**Disarm, validate, jump** (`0x23E4`–`0x246E`):

```
0023f8  str.w  r1, [r3, #0x500]      ( VBTBKR[0] = 0 )
002400  ldr    r0, [r2]              ( r2 = 0x4000 — your initial SP )
002402  and    r0, r0, #0xff000000
002406  cmp.w  r0, #0x20000000
00240a  bne.w  #0x2106               ( invalid → DFU )
00240e..00244c                        ( MSTPCRB bits 11,12 — stop USBFS/USBHS )
002450  strh   r1, [r3, #4]          ( SP monitor off )
002454  cpsid i / dsb / isb
002464  str.w  r2, [r3, #0xd08]      ( SCB->VTOR = 0x4000 )
002468  ldr    r3, [r2] ; msr msp, r3
00246e  ldr    r3, [r2, #4]          ( → your Reset_Handler )
```

Note `cpsid i` at `0x2454` and no matching `cpsie` — confirming the PRIMASK=1
handoff your `hal_entry.c:51` comment already worked around.

---

## One more thing to try for DFU access

`VBTBER.VBAE` (bit 3 at `0x4001_E4C0`) is the *VBATT backup register access
enable* bit. If it's clear, `VBTBKR` retention is not guaranteed even though
reads and writes appear to work. Worth one attempt before giving up on a
CLI route into DFU:

```
hex
A502 4001E3FE s!      ( unlock PRC1 )
4001E4C0 c@ .h        ( check VBAE first — bit 3 )
08 4001E4C0 c!        ( VBTBER.VBAE = 1 )
07738135 4001E500 !
A500 4001E3FE s!
4001E500 1 dump       ( expect 35 81 73 07 )
reboot
```

All RAM/register writes, all undone by a power cycle. If this still boots the
app, accept that the button is the only path into DFU on these boards — and on
the C board that path is blocked by the hang above until the hardware question
is answered.

---

## Where this leaves you

The C board is not damaged and its firmware is fine. It hangs in the
bootloader's `SystemInit` waiting on a VBATT-domain status bit that never
arrives after a pin reset. Practical position:

- **Use `reboot` instead of the reset button.** It works, it's a full system
  reset, and it avoids the failing path entirely.
- **Read the four VBATT registers on both boards** — that's the experiment that
  turns this from a good theory into a fact.
- **Then look at VBATT and the sub-clock crystal** on both boards.

Fixes for when you can flash again: retarget the DFU magic in `usb_init.c:80`
from `0x2000_3FFC` to `0x4001_E500` with the PRCR dance, drop the dead write in
`hal_warmstart.c:27`, move `usart_transport_init()` ahead of the peripheral
bring-up, and call the `blink()` that `hal_entry.c:31` defines but never
invokes.

---

## Reference

| Register | Address | Bits |
| --- | --- | --- |
| `RSTSR0` | `0x4001_E410` | 0 PORF, 1 LVD0RF, 2 LVD1RF, 3 LVD2RF |
| `RSTSR1` | `0x4001_E0C0` | 0 IWDTRF, 1 WDTRF, 2 SWRF, 8 RPERF, 12 SPERF |
| `RSTSR2` | `0x4001_E411` | 0 CWSF |
| `PRCR` | `0x4001_E3FE` | unlock PRC1 `0xA502`, lock `0xA500` |
| `VBTCR1` | `0x4001_E41F` | 0 BPWSWSTP |
| `SOSCCR` | `0x4001_E480` | 0 SOSTP |
| `VBTCR2` | `0x4001_E4B0` | |
| `VBTSR` | `0x4001_E4B1` | 0 VBTRDF, 1 VBTBLDF, **4 VBTRVLD** |
| `VBTBER` | `0x4001_E4C0` | 3 VBAE |
| `VBTBKR[0]` | `0x4001_E500` | 512 byte-wide, VBATT domain |

Bootloader landmarks: Reset_Handler `0x1EDC`, VBTRVLD spin `0x1EFC`, `main`
double-tap check `0x20E2`, DFU entry `0x2106`, normal boot `0x23BE`, jump to app
`0x2454`. Code ends at `0x313F`.

---

## Sources

- [arduino/arduino-renesas-bootloader — `src/main.c`](https://github.com/arduino/arduino-renesas-bootloader/blob/main/src/main.c)
- Register offsets and bitfields from `ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/R7FA4M1AB.h`; the VBTRVLD wait from `ra/fsp/src/bsp/cmsis/Device/RENESAS/Source/system.c`.
