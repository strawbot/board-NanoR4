# USB device stack — handoff

**Status: enabled, mostly working, one open issue.** `USB_TRANSPORT_ENABLE` is
`1` in `Board/usb_init.c`. The board-hanging interrupt storm that made this
`0` for a long time is fixed and confirmed stable across many tests. What's
left: the device reaches USB Address state cleanly and then goes silent —
enumeration never completes, so nothing shows up as a named device on the
host. See "Open issue" below before picking this back up.

This note has been rewritten from scratch after a long debugging session
that found and fixed three separate, real bugs. The original version's
root-cause theory (below, under "Retracted") turned out to be wrong; keeping
it here as a marked dead end so nobody re-derives it.

---

## Three bugs found and fixed, in the order they were found

### 1. The vector table pointed at the wrong handler

`ra_gen/vector_data.c` (RASC-generated) wired slot 6 (`USBFS_INT`) to
`usbfs_interrupt_handler` — the Renesas-native `r_usb_basic` driver's own
ISR (`ra/fsp/src/r_usb_basic/src/hw/r_usb_mcu.c`), from a dead, unused,
host-mode-configured stack still sitting in RASC's Stacks tab — instead of
our TinyUSB glue (`usbfs_int_isr` in `Board/usb_init.c`). Every real
interrupt was silently going to the wrong function. This is why, for a long
time, `usbfs_int_isr`'s own fire counter read 0 no matter what happened on
the bus, even while register-level activity (`INTSTS0` showing real `VBSTS`/
`DVSQ` transitions) proved the peripheral was alive.

**Fix:** excluded `r_usb_basic`/`r_usb_pcdc`/`r_usb_composite` from the
PlatformIO build entirely (`platformio.ini` — dead code, and removes the
symbol collision), then repointed `ra_gen/vector_data.c` slot 6 at
`usbfs_int_isr`, dropping the now-unused RESUME/FIFO0/FIFO1 vector slots
(7/8/9) that only existed for `r_usb_basic`'s architecture. This is a
generated file (`/* generated pin source file - do not edit */`-style
header) hand-patched the same way as the pin fixes below — it will revert if
RASC regenerates vectors, at which point re-verify the dead stack situation
in the Stacks tab first (see #2).

### 2. Two required pins, and one register bit, were never configured

`ra_gen/pin_data.c` never assigned:

- **P407 → `IOPORT_PERIPHERAL_USB_FS` (VBUS sense).** Without it the VBUS
  comparator has no input; confirmed via `INTSTS0.VBSTS` reading 0 forever
  even with a powered cable connected.
- **P914/P915 → `IOPORT_PERIPHERAL_USB_FS` (D+/D-).** Confirmed via
  `ra_cfg.txt` (RASC's own pin-function table for this part): both listed
  "Disabled".

And `usb_init.c` never set **`USBMC.VDCEN`** (transceiver regulator enable).
Without it, `SYSCFG.DPRPU` can read back "set" in software while the D+
pull-up drive is electrically marginal.

All three were found by diffing against TinyUSB's own official reference
port for this exact board — `tinyusb/hw/bsp/ra/boards/uno_r4/` (vendored
submodule, same RA4M1 die as the Arduino UNO R4 Minima this board is built
around) — which configures all three and nothing else USB-specific that we
were missing. `tinyusb/hw/bsp/ra/family.c`'s `board_init_after_tusb()` is
where the reference sets `VDCEN`, gated `#if defined(BOARD_UNO_R4)`.

**Fix:** added both pins to `ra_gen/pin_data.c` (same "hand-patched
generated file" caveat as above — also add them in RASC's Pins tab so they
survive a regen), and added the `VDCEN` write to `usb_transport_init()`
right after `tusb_init()` returns, matching the reference's placement.

**Effect:** this is what took the device from "host never notices anything,
ever" to "host runs a real bus reset" — i.e. it converted the *cold-boot
silence* problem into the *DFU-restart storm* problem happening on every
boot instead of only after a DFU handoff. Necessary, but revealed bug #3.

### 3. The ICU's per-vector interrupt latch was never cleared

This was the actual interrupt storm — confirmed via a live SWD/J-Link
session (see "Debugging setup" below) to be a **true, fast, self-sustaining
re-entry storm**: `usbfs_int_isr` firing millions of times a second with
`INTSTS0` **and** `INTENB0` both reading `0` — meaning the RUSB2 peripheral
itself had nothing enabled and nothing pending, so it could not have been
the one generating these requests.

The real mechanism: `NVIC_ClearPendingIRQ()` only clears the NVIC's own
pending latch. The RA ICU sits underneath it with a **separate** per-vector
latch, `R_ICU->IELSR[n].IR`, that actually drives the NVIC line. Every
normal FSP-generated ISR clears this on **every single interrupt** as
boilerplate (`R_BSP_IrqStatusClear()`, called automatically by
`R_BSP_IrqCfgEnable()` inside every `R_xxx_Open()`); our hand-rolled TinyUSB
init bypasses FSP's module system entirely and only ever called the bare
CMSIS `NVIC_EnableIRQ()`, which does not touch `IELSR`. TinyUSB's own
`dcd_int_handler()` only clears the peripheral's own `INTSTS0` — reasonably
so, since it has no way to know this ICU is different from a generic Cortex-M
NVIC. If `IELSR[6].IR` ever latched once (very plausibly during the
`usb_hw_reset()` module STOP/START cycle) and nothing cleared it before the
IRQ went live, the CPU would re-take the same already-latched request
forever.

**Fix:** `usbfs_int_isr()` now calls `R_BSP_IrqStatusClear((IRQn_Type) 6)`
after every `tud_int_handler(0)` call (order matters — after, not before, so
we're not racing a genuinely-still-pending peripheral condition). A
belt-and-suspenders one-time clear also stayed in `usb_hw_reset()`.

**Confirmed fixed:** `usbisr` reads low single digits (2-9), not millions,
across every test since, including full-speed (non-single-stepped) runs
with the widened `usb_hw_reset()` settle delay from the open issue below.

---

## Open issue: enumeration stalls at Address state

With all three fixes above in place, the device reliably reaches USB
Address state (`INTSTS0` = `0xA9`, i.e. `VBSTS`=1, `DVSQ`=2/`STATE_ADDR` —
`SET_ADDRESS` completed) and then goes **completely silent**: `usbisr` stays
flat (2-9 total fires for the whole reset→default→address sequence), no
further interrupts ever fire, the host never retries anything. macOS shows
the device only as a generic "USB Device" with a location ID — no
vendor/product info, meaning the descriptor exchange the host needs never
completes. `dfu-util -l` sees nothing.

**What's been ruled out:**

- Not a hardware fault (`fault.c`'s marker at `0x4001E504` is never set
  during this).
- Not TinyUSB's OSAL-none event queue overflowing/blocking (confirmed
  non-blocking, drops silently on overflow rather than hanging — and it
  isn't overflowing anyway given the low interrupt counts).
- Not the `usb_hw_reset()` module STOP/START settle delay (widened
  1ms→20ms, no change — see "Debugging setup" for why that delay was
  suspected).
- RUSB2's `SET_ADDRESS` handling is architecturally unusual and worth
  knowing before debugging further: `dcd_set_address()` in
  `dcd_rusb2.c` is a no-op — the hardware auto-handles the status stage
  and address latch itself. Once `DVSQ` autonomously reaches `STATE_ADDR`,
  a `DVST` IRQ fires and `process_set_address()` (`dcd_rusb2.c:626`, a
  small `static` function, likely inlined in release builds — break on
  the enclosing `dcd_int_handler` instead if targeting it directly)
  synthesizes a fake setup packet fed through `dcd_event_setup_received()`
  purely to sync TinyUSB's internal `_usbd_dev.addressed` flag. That
  fake event shares a counter (`_usbd_queued_setup` in `usbd.c`) with real
  setup packets, and there's a `TU_ASSERT(_usbd_queued_setup > 0,)` at
  `usbd.c:674` that silently drops the event (and everything downstream)
  if that counter's ever inconsistent — a plausible next thing to break on,
  though not yet confirmed as the cause.

**Next step when picking this up:** a live debugger session stepping
through `process_control_request`'s `TUSB_REQ_SET_ADDRESS` handling
(`usbd.c:822`) forward from there, watching for whether the host's
follow-up request (re-fetching the device descriptor at the new address)
ever arrives at the hardware level at all, versus being received but
mishandled by TinyUSB's internal state.

---

## Retracted: the original "output() masks interrupts" theory

The very first version of this document theorized: `output()` busy-waits
for space in `emitq` **with interrupts masked**, so a burst of USB activity
outrunning the UART inside a masked region deadlocks the board. **This was
wrong** — see `Docs/output-backpressure-design.md`, which traces the actual
(different, milder) bug: `output()`/`safeEmit()` do NOT mask interrupts
during their wait loop, and DO call `action_slice()` each iteration. The
real defect there is a lost-wakeup: `usart_emit()`'s TX chain only restarts
via an explicit `when(EmitEvent, ...)` call, and running actions doesn't
trigger that — so an idle TX chain can leave the wait spinning. That's a
real, separate, still-open (but low-severity) bug, tracked in
`output-backpressure-design.md`, not the cause of any of the three USB bugs
above. The original DFU-hang symptom that prompted that first theory is
consistent with bug #3 above (the ICU latch storm) — a real, fast,
CPU-monopolizing storm produces the same "no console, no LED, no fault"
symptom the original investigation saw, without needing an interrupt-masked
deadlock to explain it.

---

## Debugging setup

A live SWD debugger was essential to finding bug #3 — register snapshots
captured after a hang/reset cycle (see "Diagnostics" below) could not
distinguish "peripheral genuinely still has something pending" from "CPU
re-taking an already-latched, peripheral-independent request", and that
distinction was the whole bug. Getting a *reliable* debugger working was
its own long detour, worth documenting so it doesn't have to be re-solved.

### Current working setup: ST-Link + OpenOCD + command-line GDB

- **Probe:** an ST-Link (borrowed from an STM32F4-Discovery board), wired
  to `SWDIO`=P108, `SWCLK`=P300, `GND`, and — important — the ST-Link
  header's `VDD`/`VAPP` reference pin to the Nano's 3.3V. Without that
  last one the ST-Link can see the probe over USB but fails to reach the
  target at all ("unable to connect to the target").
- **`openocd.cfg`** (repo root): hand-rolled generic Cortex-M SWD config.
  RA4M1 has no official OpenOCD target script in the installed version
  (0.12.0) — only Synergy/R-Car/RZ are covered, not RA — so this defines
  a bare DAP + `cortex_m` target instead of sourcing a vendor file.
  Debug-only: no flash bank, so SWD can't program flash — keep using
  `pio run -t upload` (DFU) for that. Uses `interface/stlink-dap.cfg` +
  `transport select dapdirect_swd` (the older `interface/stlink.cfg` uses
  the legacy HLA driver, which doesn't support plain `swd`/needs
  `hla_swd` instead — wrong transport for a generic `cortex_m` target).
  `adapter speed` is 100 kHz — 1000 kHz produced intermittent
  "undefined debug reason"/"Failed to read memory" errors on this
  improvised jumper-wire connection; dropping it fixed that.
- **`platformio.ini`**: `debug_tool = custom` + `debug_server = openocd -f
  openocd.cfg`, `debug_port = localhost:3333`. `debug_init_cmds` does NOT
  include `$LOAD_CMDS` — attach-only, matching the no-flash-bank point
  above.
- **The critical fix: a `+0x08000000` symbol offset.** RA4M1's SWD debug
  interface reports code addresses through a `0x08000000`-based alias;
  this project's linker script (correctly) places everything at the real
  `0x00000000`-based addresses execution actually uses (see
  `memory_regions_custom.ld` — RAM at `0x20000000`, flash starting near
  `0x00004000`, no `0x08000000` anywhere). Without correcting for this,
  every breakpoint/backtrace/symbol lookup shows raw, unresolvable
  addresses ("`??`" everywhere, "corrupt stack", no symbol matches) even
  though the target is running completely normal code — confirmed by
  taking a captured PC value, subtracting `0x08000000` by hand, and
  running it through `addr2line`, which resolved cleanly. This produced
  a *lot* of false leads before being found: hours spent suspecting
  binary/symbol mismatches, missing `-g`, stale build artifacts, and
  general target instability, when the connection and the running
  firmware were fine the whole time.
  - **Command-line GDB:** `add-symbol-file <elf> -o 0x08000000` *before*
    `target remote`.
  - **VS Code/PlatformIO debug:** `debug_init_cmds` runs
    `add-symbol-file $PROG_PATH -o 0x08000000` right after connecting.
    GDB also auto-loads the plain-addressed symbol table from launching
    with the `.elf` on its command line — redundant but harmless, since
    only the offset copy will ever match a live address.
- **`-g` is required in `build_flags`.** Without it, no build all session
  ever had debug symbols at all (`-Os`, no `-g`) — a second, independent
  reason early attempts showed "No debugging symbols found" / `??`,
  compounding the offset confusion above. `-g` doesn't change the
  generated code (same optimization, same addresses) — it only adds
  DWARF sections to the `.elf`, which inflates PlatformIO's "Checking
  size" flash-percentage report into nonsense (400%+) because it naively
  sums every section including non-loaded debug ones. Ignore that number;
  check `firmware.bin`'s actual file size (what DFU actually flashes) if
  in doubt.
- **A debugger changes the bug's behavior — this is a clue, not just an
  inconvenience.** Single-stepping slowly makes real forward progress
  (hits breakpoints deep in the stack, `usbisr` climbs higher); running at
  full speed reproduces the storm/stall. That's the signature of a
  hardware-settling-time race, which is what led to finding bug #3 (an
  ICU latch race) and to widening the module STOP/START delay (which
  didn't turn out to be the remaining cause, but was a reasonable
  suspect for the same reason).
- **Stale debug-backend processes cause real, confusing symptoms** — a
  `JLinkGDBServer`/`openocd`/`arm-none-eabi-gdb` left running from a
  previous session (closed uncleanly, or whose Stop/Restart controls
  stopped responding) holds the probe's USB connection and/or GDB port
  open, so the next launch can't cleanly acquire either. Symptoms ranged
  from needing to physically unplug/replug a probe, to a session that
  connects but whose controls do nothing, to VS Code's debug panel
  vanishing on a window reload while the backend keeps running invisibly.
  `Scripts/kill_stale_debuggers.py` (wired as a `pre:` build/debug script
  in `platformio.ini`) now kills all of these unconditionally before
  every build, so this shouldn't recur.
- **VS Code's own debug UI was unreliable independent of the connection**
  — Stop/Restart not responding, the panel disappearing on reload — even
  after stale processes were cleared. The OpenOCD/ST-Link connection
  itself tested reliably every time driven directly. If VS Code's debug
  UI acts up again, command-line GDB (two terminals: `openocd -f
  openocd.cfg` in one, `arm-none-eabi-gdb` in the other) is the fallback
  that's actually been proven to work, and doesn't depend on VS Code
  extension state at all.
- **The J-Link probe this project used before the ST-Link pivot** (S/N
  `58008473`, hardware V8.00, firmware compiled Nov 2014) needed a
  physical replug before nearly every debug session, even after the
  stale-process fix. `SEGGER J-Link Commander`/`ShowFWInfo` offered no
  firmware update for it — this old hardware appears to be past what
  current tooling will auto-update. Not pursued further once the ST-Link
  proved reliable; revisit only if the ST-Link becomes unavailable.
- **Known debugger artifact, not a firmware bug:** flashing a fresh image
  and hitting Run sometimes lands in `NMI_Handler` (stack: `NMI_Handler` →
  `SystemRuntimeInit` → `SystemInit` → `Reset_Handler`, i.e. during early
  clock/system init, before `main()`). This reproduces even with USB
  untouched, so it isn't related to any of the above. Workaround: use the
  debugger's own restart control (not a fresh flash-and-run), then
  continue — lands cleanly in `main()` every time. Likely a probe
  reset/power-sequencing quirk interacting with RA4M1 clock startup, not
  investigated further since the workaround is reliable. `NMI_Handler`
  now has a real fallback for genuine unhandled NMI sources regardless
  (see `bsp_group_irq.c` below) — this artifact is a separate, cosmetic
  debugger-attach quirk, not something that fallback needs to catch.
- **Recovering a genuinely hung board without a debugger attached:** the
  bootloader's physical double-tap-reset DFU entry is independent of
  firmware state — use it to get back to `dfu-util` even if the app never
  reaches a CLI prompt.

### `NMI_Handler` now has a real fallback (`ra/fsp/src/bsp/mcu/all/bsp_group_irq.c`)

An NMI source firing with no registered callback used to be silently
dropped — harmless for a source that's merely unexpected, fatal for one
whose underlying condition can't resolve itself before the handler
returns (e.g. `BSP_GRP_IRQ_MPU_STACK`, if the stack pointer is genuinely
out of the MPU's guarded range: even the exception-return epilogue needs
that same bad stack, so the handler re-faults immediately on return and
the CPU is stuck forever). Now: any unhandled NMI source captures
`MSP`/`PSP`/`MPU->CTRL`/`MPU->RNR`/all 8 MPU region `RBAR`/`RASR` pairs
into VBATT backup registers (readable after reboot: `hex 4001E538 22
dump`), then either calls `NVIC_SystemReset()` (no debugger attached — a
call needing only a little of the current stack, no exception-return
epilogue, can escape even a genuinely broken stack) or parks in a `NOP`
loop instead (debugger attached — `NVIC_SystemReset()` mid-session was
observed to leave VS Code's debug UI stuck even after the backend
process had already exited; parking lets the debugger halt/reset via SWD
on its own terms instead). This is a hand-patch to a vendored FSP file,
not project-generated — won't get silently overwritten by RASC, but flag
it if this FSP version ever gets upgraded.

---

## Diagnostics available (CLI words, `Board/usb_init.c`)

| Word | What it shows |
| --- | --- |
| `usbisr` | Live count of `usbfs_int_isr` fires this boot. Millions = storm; low single digits during real enumeration = healthy. |
| `usbinit` | Whether `tusb_init()` reported success, and `INTENB0` immediately after it returned (catches a silent `TU_ASSERT` bail-out before `dcd_init()`, given `CFG_TUSB_DEBUG` is 0 and nothing gets logged either way). |
| `usbcrash` | Survives a hang + power cycle via VBATT backup registers: ISR count / `INTSTS0:INTENB0` / `emitq` depth, captured every 256th ISR entry plus once on the very first entry (separate slot — the one-shot write has no settling delay before the VBATT domain's slow clock commits it, so it can silently fail to retain; the periodic one gets many chances to stick). Was essential before a debugger was available; now mostly superseded by live breakpoints for anything the debugger can reach. |

`fault.c`'s existing diagnostics (below) are unrelated to USB but were used
to rule out a hardware fault as the storm's cause.

| What | Where | How to read it |
| --- | --- | --- |
| Fault handler | `Board/fault.c` | 20 LED pulses; `hex 4001E504 2 dump` for marker `0xFA017ED0`, PC, LR, xPSR, CFSR, HFSR |
| Boot watchdog | `Board/fault.c` | 15 LED pulses; `hex 4001E520 2 dump` for marker `0x0B007D06`, PC, LR. Armed in `hal_entry`, disarmed before the run loop |

All the VBATT stashes depend on `VBTBER.VBAE` being set first — without it
the backup registers accept writes and read back correctly but retain
nothing. `fault.c` uses byte offsets 4-27 in `VBTBKR[]`; `usb_isr_snapshot()`
uses 32+ to stay clear of it.

---

## Reference — registers this work depends on

| Register | Address | Notes |
| --- | --- | --- |
| `PRCR` | `0x4001_E3FE` | PRC1 unlock `0xA502`, lock `0xA500` |
| `VBTBER` | `0x4001_E4C0` | bit 3 `VBAE` — must be 1 for `VBTBKR` retention |
| `VBTBKR[0]` | `0x4001_E500` | bootloader double-tap magic `0x0773_8135` |
| `USBFS SYSCFG` | `0x4009_0000` | bit 0 `USBE`, bit 4 `DPRPU` |
| `USBFS INTENB0` | `0x4009_0030` | bits: `VBSE`=15, `RSME`=14, `SOFE`=13, `DVSE`=12, `CTRE`=11, `BEMPE`=10, `NRDYE`=9, `BRDYE`=8 |
| `USBFS INTSTS0` | `0x4009_0040` | bit 7 `VBSTS` (live VBUS level), bits 6:4 `DVSQ` device state (0=Powered, 1=Default, 2=Address, 3=Configured), same bit positions as `INTENB0` for the cause flags (`VBINT`=15 etc.) |
| `USBFS USBMC` | `0x4009_0000` region | bit 7 `VDCEN` — transceiver regulator enable, see bug #2 |
| `R_ICU IELSR[6]` | `0x4000_6318` | bit 16 `IR` (the true per-vector interrupt latch, see bug #3), bits 8:0 event number (`0x9B` = `EVENT_USBFS_INT`) |
| `NVIC ISER0` | `0xE000_E100` | bit 6 = `USBFS_INT` NVIC enable |
| `USBFS_INT` | IRQ 6 | `ra_gen/vector_data.c` |
| `USBFS0 D+/D-` | pins P914/P915 | `USBFS0: USBDP`/`USBFS0: USBDM` per `ra_cfg.txt` |
| `USBFS0 VBUS` | pin P407 | `USBFS0: VBUS` per `ra_cfg.txt` |
| SWD debug | P108 (`SWDIO`)/P300 (`SWCLK`) | per `ra_cfg.txt` |

Bootloader landmarks are in `Docs/board-a-vs-b-reset-probes.md`, and the
disassembled 16 KB image is at `Docs/nano_r4_bootloader_16k.bin`.
