# USB device stack — handoff

**Status: working and interrupt-driven.** `USB_TRANSPORT_ENABLE` is `1` in
`Board/usb_init.c`. The device enumerates (DFU Runtime + CDC composite) and
the CLI works over UART and USB CDC with the scheduler poll **off**. The
long-standing "interrupt stops being delivered" wedge was a lost wakeup in
the ISR, found by reading the RA4M1 manual and fixed 2026-10-20 — see
"Root cause: lost wakeup in the ISR (fixed and verified)". The earlier
bug #4 section and the native-stack / scope sections below are kept as the
record of how it was chased; where they say the cause was unknown, this
supersedes them.

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

## Bug #4 (found and fixed): NVIC never redelivers the USBFS interrupt

With the three fixes above in place, the device reliably reached USB
Address state and then went **completely silent**: `usbisr` stuck flat at
2 (VBUS-attach, then Resume — nothing else, ever), no further interrupts,
host never retried anything, `dfu-util -l` saw nothing.

Live register inspection (via the direct-serial CLI workflow, not a
debugger — see "Debugging setup") eventually found the actual state:
`INTSTS0` genuinely latches `DVST`/`CTRT`/`SOFR` for real, later bus
events — the device really does reach `DVSQ`=Address, a real SETUP packet
really does arrive with `VALID`=1 — but **NVIC never delivers an interrupt
for any of it**. Checked and ruled out, all clean:

- `INTENB0` stays correctly `0xDD00` throughout.
- The vector table correctly maps slot 6 to `ELC_EVENT_USBFS_INT` (`0x33`)
  — confirmed against `IELSR[6]`, which reads exactly `0x33`.
- NVIC's `ISER0` shows IRQ6 enabled; `ISPR0` (pending) and `IABR0` (active)
  both read 0 — not stuck-pending, not stuck-active.
- The ICU's own per-vector latch, `IELSR[6].IR`, reads 0 — nothing latched
  at that level either.
- Nothing in this codebase clears any of the above except
  `usbfs_int_isr()` itself (on a real entry) and the one-time clear in
  `usb_hw_reset()` before `tusb_init()` even runs.

In short: every software-visible configuration point is correct, and the
interrupt *request* from the peripheral to the ICU for these later events
genuinely isn't reaching the CPU. Pinning down why would need a working
SWD debugger or a scope on the request line — this session's debugger
attempts were abandoned (see "Debugging setup") in favor of a pragmatic
fix.

**Fix:** poll `tud_int_handler(0)` directly from the scheduler
(`Board/usb_init.c`'s `usb_action()`), guarded by
`dcd_int_disable(0)`/`dcd_int_enable(0)` so a real interrupt firing
mid-call can't re-enter `dcd_int_handler`'s shared state concurrently, in
case NVIC delivery ever recovers on its own:

```c
static void usb_action(void) {
    dcd_int_disable(0);
    tud_int_handler(0);
    dcd_int_enable(0);
    tud_task();
    later(usb_action);
}
```

This drains and dispatches whatever's pending in `INTSTS0` through the
driver's own existing, correct logic — `DVST`/`DVSQ` through the
`process_bus_reset`/`process_set_address` switch already in
`dcd_int_handler`, `CTRT` through `process_setup_packet` for real control
transfers — rather than the narrow SET_ADDRESS-only polling hack tried
earlier in this investigation. `process_set_address()` (`dcd_rusb2.c`) is
back to `static` and reached only through that normal dispatch; its one
functional addition is an explicit `DCPCTR`=`PID_BUF` re-arm plus a
`VALID`-bit clear, belt-and-suspenders in case usbd.c's own status-stage
completion doesn't run for a request this hardware already answered
autonomously at the wire level.

Confirmed fixed: `usbfs_isr_count` climbs normally now (real BRDY/CTRT
traffic), and the device fully enumerates — `dfu-util -l` reports
`Found Runtime: [cafe:4003] ... name="TimbreOS DFU", serial="ActiveRobot"`,
and `ioreg -p IOUSB -l` shows the complete device record (`idVendor` 51966
= `0xCAFE`, `kUSBAddress` set, product/serial strings all read back
correctly).

**Still open:** why NVIC stops redelivering this specific interrupt after
two real entries is unresolved. If CDC or another class needs tighter
latency than scheduler-pass polling provides (the polling call runs once
per `later()` cycle, not synchronously with bus events), revisit this with
a working hardware debugger.

### Interrupt delivery keeps wedging, not just once at boot

Live experiment (via a diagnostic `usbpoll` CLI word that flips whether
`usb_action()` calls `tud_int_handler()` at all, added temporarily to test
this — see `Board/usb_init.c`): with polling switched off mid-session,
`usbfs_isr_count` climbed on its own (real NVIC-delivered interrupts), but
the very next CDC command got no response and the count went flat again —
wedged a second time. Switching polling back on immediately un-wedged it.

So the earlier framing ("stuck forever after the first 2 events") was
incomplete: it's not a one-time startup problem, it's a **recurring**
stall that the scheduler poll has to keep clearing, for the life of the
board. The poll is not a bootstrap crutch that becomes unnecessary once
past enumeration — it is continuously load-bearing.

### Root cause: lost wakeup in the ISR (fixed and verified)

Reading the ICU and USBFS chapters of the RA4M1 manual
(`Docs/r01uh0887ej0110-ra4m1.pdf`, Rev 1.10) turned up three facts that fit
every symptom above:

- **Fig. 27.11**: `USBFS_USBI` (ELC event `033h`) is the **OR of every
  enabled `INTSTS0` status bit** through an "edge/level detector". It makes
  one request per rise of that aggregate. If any enabled bit is still set
  when the ISR returns, the aggregate never falls and no later event — on
  any bit — can produce a new request. This is the exact state captured
  live earlier: `INTSTS0 = 0x38A9` (`DVST`, `CTRT` latched), `IELSR[6].IR`
  = 0, NVIC pending = 0, `INTENB0` correct.
- **§13.2.6 (IR flag)**: the documented clear sequence is (1) negate the
  input interrupt signal, (2) read the peripheral once and wait 2 module
  clocks, (3) write 0 to `IR`. Our ISR called `tud_int_handler()` (one
  `INTSTS0` read, clears only the bits it saw) and then cleared `IR`.
  Anything that set in between was either left latched or had its edge
  erased by the `IR` clear → wedge. The scheduler poll "fixes" it simply by
  running the handler a second time, which clears the stuck bits and lets
  the aggregate fall — which is why it un-wedges instantly and why it was
  needed forever, not just at boot.
- **§27.3.3.5**: a latched `CTRT` sequence-error status (`CTSQ` = 110b)
  suppresses further `CTRT` interrupts until software clears it, a second
  "stuck until cleared" path of the same kind.

Also corrected by the manual: Fig. 27.15 shows **SetAddress execution sets
`DVST`** (Default→Address). The earlier claim in this doc and in
`dcd_rusb2.c` that the silicon handles SET_ADDRESS "autonomously with no
interrupt" was wrong; the interrupt just never got delivered. (The
autonomous part is true only of the wire transaction and `USBADDR` latch.)

**Fix** (`usbfs_int_isr()` in `Board/usb_init.c`): loop { service via
`tud_int_handler()`, read `INTSTS0`, 4×`NOP`, clear `IELSR[6].IR`, re-check
`INTSTS0 & INTENB0 & 0xFF00` } up to 8 passes. `usbisr` prints entries,
**re-passes**, spurious entries, give-ups (and the stuck registers if it ever
gives up), poll state and poll fallbacks.

**Verified on hardware (2026-10-20), scheduler poll OFF:**

| Test | Result |
|------|--------|
| Interrupt-only enumeration, 10 consecutive resets (`reboot` over CDC) | 10/10 mounted within ~2.5 s; 0 fallbacks, 0 give-ups |
| Re-passes per enumeration | ~28–33 of ~115 ISR entries, every boot — the race fires constantly, the old ISR was losing those wakeups |
| CDC command loop, 3 × 24 s | 57/57 answered |
| Large output: `help` (12,242 B) × 15 | 15/15 identical length (~184 KB), 0 give-ups |

One unexplained early failure: the very first poll-off run after a fresh
flash got 0/12 answers (before the counters existed). It never recurred in
~80 later trials across several flashes; if it does, the `give-ups` /
last-give-up register dump in `usbisr` is there to catch it.

**Poll is now OFF by default.** `usb_action()` only pumps `tud_task()`.
`usb_poll_fallback()` is a guarded safety net: if VBUS is present but USB
has not mounted 15 s after boot it turns the scheduler poll on and counts it
(`poll fallbacks` in `usbisr`) so a board whose only link is USB can't strand
itself. `usbpoll` still toggles the poll for experiments (turning it off arms
a 30 s auto-restore).

**8-hour soak (2026-10-21), poll OFF, CDC + UART watchdog:** 5,648 short
commands, 474 bulk `help` dumps (12 KB each), and 47 periodic resets
(`reboot` over UART every 10 min, each required to re-enumerate and answer on
interrupts alone) — **0 failures**. Worst reset-to-answer 3.2 s; `give-ups` 0
and `poll fallbacks` 0 on every check; at the end the board showed 6,674 ISR
entries, 1,051 re-passes (~16 %), 0 give-ups, 0 fallbacks. (The harness
captures the USB/ICU/NVIC registers over the UART and stops on 3 consecutive
CDC failures; it never triggered.)

**Hot-plug test (2026-10-21), poll OFF, board powered from VIN throughout:**
12 unplug/replug cycles at the board end (flicks to ~13 s, some mid-enumeration)
— **12/12 re-enumerated and answered with no reset** (board uptime kept
counting), `give-ups` 0, `poll fallbacks` 0, 2,117 ISR entries / 526 re-passes
by the end. One cycle took ~8.6 s to answer instead of ~1.3 s (macOS retried
part of the enumeration after a replug mid-handshake; the board recovered by
itself). Caveat on what this exercised: with VIN supplying the board's 5 V rail
the VBUS-sense pin stays high (`VBSTS` = 1) when the cable is pulled, so the
board sees *suspend, then a bus reset on replug* (`DVSQ` 7 → 1 → 3; `DPRPU`
stays 1) — **not** the VBUS-drop interrupt (`VBINT`/`dcd_disconnect`). That
path can't be hit on this board as wired without losing power; with USB as the
only supply an unplug is a power-off, i.e. the cold-boot case. (Measurement
note for the harness: it spends ~6.8 s probing over the UART after seeing the
node vanish, so its "unplugged" and "answered in" times are floored at ~6.8 s
and ~1.3 s respectively; pass/fail, reset detection and counters are exact.)

**Still to do:** a genuine cold power-on (USB only, no DFU handoff — also the
only way the board ever sees VBUS appear); a run under the web-page/NCM
streaming load; once confident, delete the scheduler poll and
`usb_poll_fallback()` entirely.

**Related, not USB:** a "USB device disappears" report on 2026-10-21 turned out
to be `R_IOPORT_Open(…, NULL)` in `Board/analog_test.c` — with FSP parameter
checking off project-wide that loops over 10,712 garbage "pin configs" read
from flash and writes arbitrary peripheral registers. Fixed there; if USB ever
drops erratically again, check for NULL/unopened FSP handles first.

### Unique USB serial number (two boards on one host)

The USB serial string was the constant `"ActiveRobot"`, so two boards had
identical VID/PID/serial and macOS showed only one of them. It is now
`"ActiveRobot"` + 3 hex digits hashed from the RA4M1's factory-unique 128-bit
ID (`usb_serial_string()` in `Board/usb_init.c`, `R_BSP_UniqueIdGet()`), e.g.
`ActiveRobot82C`.

The **length matters** because macOS builds the CDC node name from the serial
(`/dev/cu.usbmodem<serial><interface>`), which makes the right port easy to
pick out among many: the board above appears as
`/dev/cu.usbmodemActiveRobot82C2`. macOS only does this for serials up to
**14 characters** (observed: 14 works, 19 and 32 do not — it silently falls
back to a location-based `usbmodem13102`-style name). `ActiveRobot` + 3 digits
is exactly 14, which is why the uniquifier is only 12 bits: plenty to tell
boards on one bench apart (collision odds for 3 boards ≈ 0.07 %), but if two
boards ever hash to the same value, bump `USB_SERIAL_UNIQ_DIGITS` knowing the
node name will then lose the serial. `dfu-util -l` / `ioreg` show the serial
either way.

A board only changes identity once reflashed with this build; boards still
on old firmware keep the shared `"ActiveRobot"` serial and will collide with
each other.

### SCKE/USBE ordering fixed (real bug, likely not the cause of the above)

Diffing our hand-rolled bring-up against RASC's native `r_usb_basic` driver
(`hw_usb_pmodule_init()`, `ra/fsp/src/r_usb_basic/src/hw/r_usb_preg_access.c`)
found `usb_hw_reset()` (`Board/usb_init.c`) clearing `SYSCFG.USBE` before
`SYSCFG.SCKE` (USB clock enable) had ever been set this boot — the manual
(27.2.1, Note 2) requires SCKE=1 before USBE may be changed at all. Fixed:
`usb_hw_reset()` now sets and confirms SCKE right after
`R_BSP_MODULE_START()`, before touching anything else in `SYSCFG`, matching
both the native driver and TinyUSB's own `dcd_init()`.

Caveat: every boot tested this session has been a DFU handoff (bootloader
→ app), where SCKE was almost certainly already 1 (inherited from the
bootloader's own live USB session) — so this bug likely hasn't been in
effect in anything actually tested here. A genuine cold power-on boot is
the untested case it would matter for. Worth having fixed regardless (it's
a documented sequencing violation), but it did not change the wedging
behavior above, tested only via DFU-handoff boots.

Also found via the same diff: RASC's generated `g_basic0_cfg`
(`ra_gen/hal_data.c`) configures IRQ priority **12** for the USB vector
(`.ipl = 12`); our TinyUSB integration never calls `R_BSP_IrqCfgEnable()`
or `NVIC_SetPriority()` for it at all, leaving it at the register's reset
value of 0 (highest priority).

**Tried and ruled out:** added `NVIC_SetPriority((IRQn_Type) 6, 12)` in
`usb_transport_init()` (`Board/usb_init.c`), right after the VDCEN fix,
matching RASC's intended priority exactly (confirmed live: `IPR[6]` reads
`0xC0`, i.e. `12 << 4`, matching the other FSP-managed peripherals' own
`0xC0` priority). Re-ran the same `usbpoll`-off wedge test: ISR count
still climbed a few entries (58→62) then flat-lined, CDC still got no
response, and re-enabling polling still immediately un-wedged it —
identical behavior to before the fix. Priority was not the cause. Left the
fix in place anyway (it's still correct — matches RASC's own intended
config, and running the USB interrupt at the highest possible NVIC
priority for no reason was never intentional), but it does not explain or
fix the wedging.

### Native stack smoke test

Question: does RASC's native `r_usb_basic` + `r_usb_pcdc` driver have the
same interrupt-delivery problem, or is it specific to our TinyUSB
integration? Set up a standalone `[env:nano_r4_usbtest]` PlatformIO
environment (`platformio.ini`) that swaps TinyUSB for the native stack via
`-D USE_NATIVE_USB_STACK`:

- `Board/usb_native_test.c`/`.h` — opens `g_basic0`-equivalent config
  (RASC had already generated `g_basic0_cfg` in `ra_gen/hal_data.c` for
  `USB_CLASS_PCDC`, just never called `R_USB_Open()` against it), with a
  thin ISR wrapper (`usbfs_native_isr`, mirroring `usb_init.c`'s
  `usbfs_int_isr`) counting real interrupt entries, and an app callback
  counting state-transition events (`USB_STATUS_POWERED`/`DEFAULT`/
  `ADDRESS`/`CONFIGURED`/...).
- `Board/usb_native_test_descriptor.c` — RASC's own
  `r_usb_pcdc_descriptor.c.template`, never previously instantiated,
  copied in with a distinct VID/PID (`0xCafe`/`0x4005`).
- `Board/usb_init_stubs.c` — stand-ins for `usb_init.c`'s CLI words
  (`boardwords.txt` is shared between both environments) and for
  `tud_dfu_runtime_reboot_to_dfu_cb` (the `dfu` CLI word), so this
  environment can still be reflashed via `dfu-util` without a physical
  double-tap reset.
- `ra_gen/vector_data.c`/`.h` — vector slot 6 conditionally wired to
  `usbfs_native_isr` instead of `usbfs_int_isr`.

Needed the same `USBMC.VDCEN` fix TinyUSB's build needed (not set by
`R_USB_Open()` any more than by TinyUSB's `dcd_init()`) before the native
stack generated any interrupt activity at all — confirming VDCEN really is
a board-level requirement, not something specific to either stack.

**Result:** with VDCEN set, `usbfs_interrupt_handler` (the native ISR) fired
in the same bursty, plateauing pattern TinyUSB showed (8 → 11 → *flat* → 14
→ *flat*, sampled once a second) — not a clean, continuously-climbing
count. The application callback (`p_usb_apl_callback`) never fired even
once across multiple boots, despite over a dozen real ISR entries — the
native driver's own internal state machine never reached even
`USB_STATUS_POWERED`. The host never saw the device at all (no new
`/dev/cu.usbmodem*`, nothing in `ioreg`/`dfu-util -l`).

This is evidence **against** the bug being specific to our TinyUSB
integration: two independently-written driver stacks, sharing only the
silicon, the board bring-up (pins, clocks, VDCEN), and the physical
USB connection, both show the same bursty/wedging interrupt behavior and
both fail to reach host-visible enumeration reliably. That points at
something more fundamental — signal integrity on the physical D+/D-/VBUS
connection, a clock configuration issue upstream of both stacks, or a
genuine RA4M1 erratum — rather than a fixable defect in either driver's
software. **Do not expect switching to the native stack to solve this** on
its own; the smoke test's own descriptor/glue is minimal, so it can't rule
out a native-side bug in what wasn't exercised (real host requests beyond
initial attach), but the ISR-level wedging is directly comparable and
matches.

Restored to the working TinyUSB build (`env:nano_r4`) after this test —
the `env:nano_r4_usbtest` environment is left in the tree for whoever picks
this up next, in case a hardware-level investigation (scope/logic analyzer
on D+/D-/VBUS, or a genuine cold power-on boot rather than only
DFU-handoff boots) is the next real step.

### Scope investigation: D+/D-/VBUS look electrically healthy

Followed up on "scope/logic analyzer on D+/D-/VBUS" above using a Digilent
Analog Discovery 3 (2 analog channels on D+/D-, one digital channel on
VBUS), probed inline via a USB-C breakout with common ground. Two short
captures (triggered on a D+/D- edge, ~2 MS/s, a few ms to tens of ms
window — see "Scripting the scope" below for how) around a board reboot
found:

- **Idle levels are clean and correct**: D+ sits at a stable ~3.3V, D- at
  ~0V, both before and after the reboot event — proper USB Full-Speed idle
  "J" state, no marginal/drooping voltage that would suggest a flaky
  connector, bad solder joint, or underdriven pull-up.
- **Real packet-like activity is visible right at the event**: two bursts
  of rapid, aliased transitions (the scope's ~2 MS/s can't resolve
  individual 12 Mbit/s NRZI bits, so real data shows as chaotic
  intermediate voltages rather than clean edges) about 125 µs apart,
  consistent with a genuine token/data exchange on the wire, with a clean
  return to idle afterward.
- **Could not get a long enough capture to see the repeated wedge/recovery
  pattern**: WaveForms' Record mode, on this device, capped every attempt
  (scripted or manual, analog+digital or analog-only) at ~1.5 ms regardless
  of the configured duration/rate — a real constraint in this device/mode
  combination, not a scripting mistake. Confirming whether a *second*
  reset/retry cycle correlates with the firmware-side wedging above would
  need either a different capture strategy (e.g. an external trigger pulse
  from a spare GPIO at the moment `usbfs_int_isr` fires, single-shot
  triggered on that instead of free-running Record) or different
  instrumentation entirely (dedicated USB protocol analyzer).

Net effect: no electrical/signal-integrity red flags found in what we could
capture, which is mild further evidence (not proof) that this is a
software/interrupt-delivery problem rather than a hardware one — consistent
with the native-stack finding above.

### Scripting the scope (WaveForms, for next time)

WaveForms has a real, documented JavaScript scripting engine built into the
app itself (Instruments menu → Script; `Contents/Resources/digilent/
waveforms/doc/script.html` in the app bundle has the API reference).
Running scripts through it — rather than Digilent's separate DWF SDK
(`Contents/Frameworks/dwf.framework`, with Python/C#/VB samples under
`Contents/Resources/SDK/samples/`) called from an external process — matters
on macOS specifically: the device requires a DriverKit USB transport
entitlement (`com.apple.developer.driverkit.transport.usb`) that only a
properly signed, entitled app (WaveForms.app itself) can obtain. An
external script (Python via `ctypes`, or the SDK's own `dwfcmd` CLI tool)
loading the same library gets `FDwfEnum` returning 0 devices no matter how
many times the cable is replugged — not a timing issue, a hard platform
restriction. The in-app Script tool sidesteps this entirely since it runs
inside the already-entitled process.

Gotchas hit getting a script working, for next time:

- **Property names don't always match the docs' shorthand.** `.DigitalChannels`
  needs the actual DIO number as the child property (`Scope1.DigitalChannels.DIO6`,
  not `.channel6`) — found via the Script tool's **Picker** (hover the GUI
  element, press a key, it inserts the real property path). Picker is the
  reliable way to resolve any property-name mismatch; guessing from the doc
  text alone got this one wrong.
- **Config changes need an explicit `.config()` to take effect before
  dependent child objects exist** — e.g. `Scope1.DigitalChannels` is
  `undefined` until *after* `Scope1.Digital.checked = true; Scope1.config();`
  has actually run, not just been assigned.
- **`Time.Base` is time-*per-division*, not total span**, and only accepts
  values from `Scope1.Time.Base.preset`'s list (e.g. `"100 ms/div"`) — an
  arbitrary number or an unlisted value is silently ignored, keeping
  whatever the previous setting was. Print `.preset` before guessing a
  value.
- **macOS's per-folder privacy protection (Desktop/Documents/Downloads)
  blocks Terminal-level file reads even after `Scope1.Export()` succeeds.**
  Export directly to a path outside those protected folders (e.g. the
  project directory) to avoid it entirely.
- **10x probes need WaveForms' probe-attenuation setting to actually say
  10x** — left at the default 1x, every voltage reading is 10x too low
  (e.g. a real 3.3V line reads as ~0.33V), which looks exactly like a
  hardware signal problem until you notice it's a probe setting.
- **Record mode capped at ~1.5 ms** in this device/channel configuration no
  matter what duration or rate was requested (scripted or via the GUI
  directly) — a real constraint on this hardware/software combination worth
  knowing about before planning a long capture around it.

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

## CDC bring-up (Phase 2)

With enumeration fixed (bug #4), added CDC ACM alongside DFU Runtime so
the CLI is available over USB, duplicating the existing UART transport.

- `Board/tusb_config.h`: `CFG_TUD_CDC 1`, 256-byte RX/TX FIFOs, 64-byte
  USB packet size.
- `Board/usb_init.c`: device class changed to MISC/COMMON/IAD (`0xEF`/
  `0x02`/`0x01`) so CDC's control+data interface pair gets a proper IAD
  grouping for Windows/macOS composite-driver loading; PID bumped to
  `0x4004` (new interface set, avoid host driver-cache confusion); config
  descriptor now declares 3 interfaces (0 = DFU Runtime, 1+2 = CDC via
  `TUD_CDC_DESCRIPTOR`); fresh endpoints `EP_CDC_NOTIF`/`OUT`/`IN`
  (`0x81`/`0x02`/`0x82` — RUSB2 has 9 non-control pipes, no need to drop
  the notification endpoint the way board-Discovery does for its
  CDC+NCM combo).
- `platformio.ini`: added `tinyusb/src/class/cdc/cdc_device.c` to the
  explicit TinyUSB file list (linker fails with undefined `cdcd_*`
  references without it — `usbd.c`'s driver table references the CDC
  class driver unconditionally once `CFG_TUD_CDC` is set).
- `Robot/cli/cli_transport_cdc.c` (new, board-agnostic — talks only to
  `tud_cdc_*` and the existing `Robot/cli/cli_transport.c` bridge, no
  board registers): ported from `board-Discovery/Board/cli_transport_cdc.c`
  but adapted to route received bytes through `cli_transport_rx_byte()`
  instead of calling `keyIn()`/`when(EmitEvent, ...)` directly, matching
  the pattern this board's own `cli_transport_usart.c` already
  established. Per this board's `CLAUDE.md` ("factor into Robot/ first,
  then adopt it here"), this lives in `Robot/cli/` rather than as a
  Nano-local file, since the TinyUSB CDC glue itself has no
  board-specific content — `board-Discovery` was not migrated to it in
  this pass (out of scope, untested here), so it still has its own local
  copy.

Confirmed working: `dfu-util -l` and `ioreg` both show the composite
device (`cafe:4004`, DFU Runtime + CDC); a new `/dev/cu.usbmodemActiveRobot*`
port appears; the CLI responds correctly over it (`show-sys`, `usbisr`
etc. tested); and the auto-switch-to-last-transport mechanism (already
implemented in `Robot/cli/cli_transport.c`, unchanged here) correctly
routes each reply back through whichever of UART/CDC most recently sent a
byte — verified by ping-ponging commands between both ports in the same
session.

**Not done:** CDC-ECM (Phase 3, IP over USB) — still needs `CFG_TUD_NCM 1`
and the NCM interface descriptors, deferred per the original plan.

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
| `usbisr` | ISR entries this boot (millions = storm; ~115 after a normal enumeration), **re-passes** (ISR found an enabled `INTSTS0` bit still set after clearing the ICU latch and looped — ~25% of entries is normal), spurious entries, **give-ups** (hit the 8-pass limit; should stay 0 — if not, the last-give-up `INTSTS0/BRDYSTS/NRDYSTS/BEMPSTS` dump names the bit holding the line high), scheduler-poll state, and **poll fallbacks** (should stay 0). |
| `usbpoll` | Toggle the scheduler poll of `tud_int_handler()` (default off). Turning it off arms a 30 s timer-driven auto-restore; the last off-window's ISR/re-pass counts appear in `usbisr`. |
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
