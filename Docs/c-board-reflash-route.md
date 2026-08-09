# Getting new firmware onto the C board

Goal: change the firmware on a board whose reset button hangs it and whose DFU
you have never been able to trigger.

The short version: **you don't need DFU.** The Nano R4 has a documented recovery
path that bypasses the Arduino bootloader entirely, and it works on this board
because it's entered at power-on — which is the one thing the C board does
reliably. Chase the CLI route too, because it's cheap and it would fix the
underlying bug, but don't let it block you.

---

## Route 1 — BOOT pin + Renesas Flash Programmer (the reliable one)

The RA4M1 has a factory ROM boot mode selected by the MD pin at reset. Arduino
brings this out as a **BOOT pin, located between A0 and A5** on the Nano R4.

1. Short **BOOT to GND**.
2. Plug in USB. The board comes up in Renesas ROM boot mode and enumerates as a
   serial port — not DFU, not the Arduino bootloader.
3. Flash with Renesas Flash Programmer: select the COM port, load your image,
   press Start.

Why this works when nothing else does: boot mode is selected at **power-on**,
and the C board power-cycles fine. It never touches the reset button, the DFU
magic, or `VBTBKR`. It also programs the whole chip, so you can restore the
Arduino bootloader (`dfu_nano.hex`, shipped inside the UNO R4 core) as well as
your own application.

**macOS caveat:** Renesas Flash Programmer is Windows and Linux only. Options
are a Linux VM with USB passthrough, `rfp-cli` on a Linux box, or borrowing a
Windows machine. This is the main friction in Route 1 — but it is friction, not
a wall.

This route is also your safety net. Once you know it works, everything else
below becomes low-risk, because a bad outcome is recoverable.

---

## Route 2 — the CLI experiments worth running first

All read-only or volatile. Run these before committing to Route 1; two of them
would also explain the underlying bug.

### 2a. Is the 32.768 kHz sub-clock actually running on the C board?

This is the highest-value test on the list, and you already have the word for it.

`board_cli.c` says your RTC is "hardware IRTC on 32.768 kHz sub-clock", seeded
from `__TIMESTAMP__` by `clocks.c` at boot. So:

```
show-time
```

on both boards, twice, a few seconds apart.

| Result | Meaning |
| --- | --- |
| RTC advances on both | Sub-clock fine on both; look elsewhere. |
| RTC advances on micro, frozen on C | **The C board's 32.768 kHz crystal isn't running.** That is the same silicon corner as the VBATT domain and `VBTSR.VBTRVLD`, and it would explain the hang directly. |

The bootloader stops the sub-clock at `0x1F36` (`SOSCCR = 1`) immediately after
the `VBTRVLD` wait, which is a strong hint that the two are coupled in the
startup sequencing.

### 2b. Does `VBTBKR` survive a software reset at all?

This is the decisive test for whether a CLI route into DFU is even possible. Use
a scratch byte the bootloader never touches — it clears `VBTBKR[0..3]` on every
boot, so testing with the magic itself tells you nothing.

```
hex
A502 4001E3FE s!      ( unlock PRC1 )
DEADBEEF 4001E504 !   ( VBTBKR[4] — bootloader never touches this )
A500 4001E3FE s!
4001E504 1 dump       ( confirm it took )
reboot
```

then after the CLI comes back:

```
hex
4001E504 1 dump
```

| Result | Meaning |
| --- | --- |
| Reads back `EF BE AD DE` | Retention works across `NVIC_SystemReset`. Your magic write *should* have worked — go to 2c. |
| Reads back zeros or garbage | The VBATT backup does not survive a software reset. **No CLI route into DFU exists on these boards** — take Route 1 and stop spending time here. |

### 2c. `VBTBER.VBAE` — backup register access enable

Bit 3 of `VBTBER` at `0x4001_E4C0` gates access to the backup registers. If it's
clear, reads and writes can appear to work while retention is not guaranteed —
which would fit exactly what you saw: correct read-back, nothing after the
reset.

```
hex
4001E4C0 c@ .h        ( check VBAE bit 3 before changing anything )
A502 4001E3FE s!
08 4001E4C0 c!        ( VBAE = 1 )
07738135 4001E500 !
A500 4001E3FE s!
4001E500 1 dump       ( expect 35 81 73 07 )
reboot
```

Compare the initial `VBTBER` reading between the two boards while you're here —
a difference is itself a finding.

### 2d. Pre-condition the domain, then press the button

If 2a shows the sub-clock is the problem, this tests the theory and might fix
the button outright. VBATT-domain state survives a reset, so anything you set up
from the CLI is still there when the bootloader's `Reset_Handler` runs.

```
hex
4001E4B1 c@ .h        ( VBTSR before )
A502 4001E3FE s!
03 4001E4B1 c!        ( clear VBTRDF and VBTBLDF — write-1-to-clear flags )
A500 4001E3FE s!

A501 4001E3FE s!      ( unlock PRC0 for clock registers )
0 4001E480 c!         ( SOSCCR = 0 — start the sub-clock oscillator )
A500 4001E3FE s!
```

Wait five seconds or so — a 32 kHz crystal is slow to start — then press the
reset button.

If the board boots, the sub-clock/VBATT domain was the missing ingredient, the
`VBTRVLD` diagnosis is confirmed, and you have both a workaround and a precise
bug report. If it still hangs, power-cycle and move on; nothing here is
persistent.

### 2e. Precondition for any DFU route at all

Before assuming DFU would even help, confirm the C board's USB works when your
app is running:

```
dfu-util -l
```

Your app presents a DFU Runtime interface at PID `0x4003` (`usb_init.c:130`). If
the C board doesn't show up there, its USB is not working in the application
either, and the bootloader's DFU mode would very likely be just as invisible —
which settles the question in favour of Route 1.

---

## Route 3 — deliberately invalidate the app vector (not recommended)

For completeness. The bootloader's only validity test is at `0x2400`:

```
002400  ldr    r0, [r2]           ( r2 = 0x4000, your initial SP )
002402  and    r0, r0, #0xff000000
002406  cmp.w  r0, #0x20000000
00240a  bne.w  #0x2106            ( invalid → DFU mode, permanently )
```

Corrupt the first word at `0x4000` and the board enters DFU on every boot,
forever, with the bootloader itself untouched. That is a guaranteed way in.

It requires driving the FACI flash controller by hand from Forth, which is a
real piece of work and the one operation that can cost you the CLI with nothing
to fall back on except Route 1. Since Route 1 is simpler and already gets you
where you want to go, this is not worth doing.

---

## What to fix once you can flash

- **`usb_init.c:80`** — retarget the DFU magic from `0x2000_3FFC` to
  `0x4001_E500`, wrapped in the PRCR dance:

  ```c
  R_SYSTEM->PRCR = 0xA502U;
  *(volatile uint32_t *) 0x4001E500UL = 0x07738135UL;
  R_SYSTEM->PRCR = 0xA500U;
  NVIC_SystemReset();
  ```

  If test 2b showed retention fails across a software reset, add a read-back and
  report failure rather than resetting into a board that just boots the app
  again — a `dfu` word that silently does nothing is worse than one that says it
  can't.

- **`hal_warmstart.c:27`** — the write to `0x2000_3FFC` targets unused RAM and
  does nothing. Delete it.

- **`hal_entry.c`** — move `usart_transport_init()` ahead of the peripheral
  bring-up so the console exists before anything that can block, and call the
  `blink()` defined at line 31 but never invoked.

- **Latch the reset cause.** Read `RSTSR0`/`RSTSR1`/`RSTSR2` and `VBTSR` into
  variables at the top of `hal_entry()` and expose them as a CLI word, so a
  failing boot can be read out afterwards instead of inferred.

Per `Nano/CLAUDE.md`, the DFU-entry helper is board-agnostic in shape but
depends on RA4M1 registers — put the generic half in `Robot/` with a hook and
keep the register access here.

---

## Sources

- [Nano R4 Bootloader Recovery and Flashing](https://docs.arduino.cc/tutorials/nano-r4/bootloader-flashing/) — BOOT pin between A0 and A5, short to GND, flash with RFP, `dfu_nano.hex`.
- [arduino/arduino-renesas-bootloader — `src/main.c`](https://github.com/arduino/arduino-renesas-bootloader/blob/main/src/main.c)
- Register offsets and bitfields from `ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/R7FA4M1AB.h`; `VBTRVLD` wait from `ra/fsp/src/bsp/cmsis/Device/RENESAS/Source/system.c`. Bootloader landmarks from `Docs/nano_r4_bootloader_16k.bin`.
