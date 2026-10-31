// usb_init.c — TinyUSB task pump and composite (DFU Runtime + CDC) descriptor
// for Nano R4
//
// Hardware prerequisites (configured in RASC, NOT here):
//   USBFS peripheral; VBUS/D+/D- on P407/P914/P915 (see ra_gen/pin_data.c).
//   USBFS_INT event wired to IRQ 6 in ra_gen/vector_data.h/.c.
//
// Composite: DFU Runtime + CDC for the CLI (Board/cli_transport_cdc.c, board-
// side because it uses tusb.h, wired to the shared Robot/cli/cli_transport.c
// bridge exactly like this board's own cli_transport_usart.c) and, in
// env:nano_r4_web only (-D WITH_USB_NCM), a CDC-NCM network function for the
// web page (Board/usb_ncm.c).
//
// DFU Runtime flow:
//   $ dfu-util -l                      — lists device in Runtime mode
//   $ dfu-util -e                      — sends DETACH; device resets into bootloader
//   $ dfu-util -D firmware.bin         — (after re-enumerate) downloads new image
//
// USB interrupt (ra_gen/vector_data.c):
//   usbfs_int_isr → tud_int_handler(0)
//
// TEA integration (same pattern as board-Discovery):
//   usb_action self-reschedules via later(usb_action) so tud_task() is
//   serviced on every scheduler pass without blocking.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal_data.h"

#include "tea.h"
#include "printers.h"
#include "clocks.h"
#include "project_defs.h"

#include "cli.h"
#include "byteq.h"

#include "tusb.h"
#include "usb_init.h"
#include "board_id.h"
#include "usb_ncm.h"

// ── VBATT backup register helpers ─────────────────────────────────────────────
//
// Shared by dfu_arm_bootloader() below and usb_isr_snapshot(). See that
// function's comment for why VBAE must be set first and PRCR unlocked around
// the writes.

#define PRCR_PRC1_UNLOCK  0xA502U
#define PRCR_LOCK         0xA500U
#define VBTBER_VBAE       (1U << 3)

#define VBT_WORD(off)     (*(volatile uint32_t *) &R_SYSTEM->VBTBKR[(off)])

// ── USBFS interrupt handler ───────────────────────────────────────────────────
//
// Declared in ra_gen/vector_data.h and registered in the g_vector_table at
// slot 6 (USBFS_INT_IRQn).  The FSP ICU routes ELC_EVENT_USBFS_INT here.
//
// usbfs_isr_count — free-running fire count, read by the `usbisr` CLI word.
// See Docs/usb-handoff.md: if this climbs into the millions in a second once
// USB_TRANSPORT_ENABLE is on, the suspected ISR storm is confirmed.

static volatile uint32_t usbfs_isr_count;

// How many times usbfs_int_isr() found an enabled INTSTS0 status bit still
// set AFTER servicing and clearing the ICU latch, and went round again.
// Nonzero is direct evidence of the lost-wakeup race described in
// usbfs_int_isr() below. Shown by `usbisr`.
static volatile uint32_t usbfs_isr_repass;

// Set when the re-check loop used all USB_ISR_MAX_PASSES and an enabled
// INTSTS0 bit was STILL set: which bit(s) keep the aggregate high, plus the
// per-pipe status registers behind BRDY/NRDY/BEMP. Shown by `usbisr`.
static volatile uint32_t usbfs_isr_giveups;
static volatile uint16_t usbfs_stuck_intsts0, usbfs_stuck_brdy, usbfs_stuck_nrdy, usbfs_stuck_bemp;

// Counts of the first-pass INTSTS0 cause bits seen on ISR entry, and of
// entries with NO enabled cause bit at all (spurious / stale IR).
static volatile uint32_t usbfs_isr_spurious;

#define USB_ISR_MAX_PASSES  8U
#define USB_INTSTS0_IRQ_BITS  0xFF00U   // VBINT RESM SOFR DVST CTRT BEMP NRDY BRDY

static inline bool usb_irq_still_pending(void) {
    return (R_USB_FS0->INTSTS0 & R_USB_FS0->INTENB0 & USB_INTSTS0_IRQ_BITS) != 0;
}

// ── usb_isr_snapshot — crash telemetry that survives a hang + power cycle ────
//
// A lockup during real USB traffic leaves the CLI unresponsive, so nothing
// can be queried live once it happens. This stashes the ISR count, the raw
// USBFS INTSTS0/INTENB0 (read BEFORE tud_int_handler() touches anything —
// this is the true, unmassaged cause), and the shared CLI emitq depth into
// VBATT backup registers (fault.c's marker lives at byte offsets 4-27; this
// uses 32+ to stay clear of it).
//
// Two captures, at different offsets, answer different questions:
//   - "first" (offset 48+, written once, on entry #1): what actually
//     triggered the very first interrupt of the storm — the real cause,
//     before millions of subsequent entries could muddy it.
//   - "last" (offset 32+, overwritten every 256th entry): the state deep
//     into the storm, and how many times it had fired by then.
// If "first" and "last" show the same cause bits, whatever triggers it
// never gets acknowledged and just keeps re-arriving. If they differ, the
// storm changes character partway through. Read back with `usbcrash`.
// See Docs/usb-handoff.md.

#define USB_SNAPSHOT_MARKER  0xC0FFEE01UL

static void usb_isr_snapshot(bool first) {
    uint16_t intsts0 = R_USB_FS0->INTSTS0;
    uint16_t intenb0 = R_USB_FS0->INTENB0;
    Cell     depth   = qbq(emitq);

    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;

    if (first) {
        VBT_WORD(48) = USB_SNAPSHOT_MARKER;
        VBT_WORD(52) = ((uint32_t) intsts0 << 16) | intenb0;
    } else {
        VBT_WORD(32) = USB_SNAPSHOT_MARKER;
        VBT_WORD(36) = usbfs_isr_count;
        VBT_WORD(40) = ((uint32_t) intsts0 << 16) | intenb0;
        VBT_WORD(44) = (uint32_t) depth;
    }

    R_SYSTEM->PRCR = PRCR_LOCK;
}

void usb_cli_crash_snapshot(void) {
    if (VBT_WORD(48) == USB_SNAPSHOT_MARKER) {
        print("INTSTS0:INTENB0 at FIRST entry: "); printHex((Long) VBT_WORD(52)); printCr();
    } else {
        print("no first-entry snapshot captured since last power-up"); printCr();
    }

    if (VBT_WORD(32) != USB_SNAPSHOT_MARKER) {
        print("no last-entry snapshot captured since last power-up"); printCr();
        return;
    }
    print("usbfs_isr_count at last snapshot: "); printDec((Long) VBT_WORD(36)); printCr();
    print("INTSTS0:INTENB0 at last snapshot: "); printHex((Long) VBT_WORD(40)); printCr();
    print("emitq depth at last snapshot: ");     printDec((Long) VBT_WORD(44)); printCr();
}

void usbfs_int_isr(void) {
    usbfs_isr_count++;
    if (usbfs_isr_count == 1) {
        usb_isr_snapshot(true);
    }
    if ((usbfs_isr_count & 0xFFU) == 0) {
        usb_isr_snapshot(false);
    }

    // The ICU IELSR[n].IR clear is mandatory on every entry (the storm fix:
    // IR is a true latch, not a reflection of the peripheral's line, and
    // only software clears it). The ORDER and the re-check are what the
    // RA4M1 manual (R01UH0887EJ0110) calls for:
    //
    //  - 13.2.6, IR flag: to clear IR, (1) negate the input interrupt
    //    signal, (2) read the peripheral once and wait 2 module clocks,
    //    (3) write 0 to IR.
    //  - Fig. 27.11: USBFS_USBI is the OR of every ENABLED INTSTS0 status
    //    bit through an edge/level detector -- one request per rise of that
    //    aggregate. If any enabled bit is still set when we return, the
    //    aggregate never falls, so no later event can raise a new request
    //    (INTSTS0 shows DVST/CTRT latched, IR=0, nothing pending in NVIC --
    //    exactly the wedge in Docs/usb-handoff.md, which a scheduler poll
    //    used to mask by running this handler a second time).
    //  - 27.3.3.5: a latched CTRT sequence-error status likewise suppresses
    //    later CTRT interrupts until software clears it.
    //
    // A status bit that sets between tud_int_handler()'s INTSTS0 read and
    // the IR clear is left set, or has its edge erased by the clear. So:
    // service, clear IR, then re-check what is still pending and go round
    // again. Bounded so a stuck bit can't pin the CPU in the ISR (a hit on
    // the bound is counted and its registers captured: see `usbisr`).
    // INTENB1/INTSTS1 are host-mode
    // only (INTENB1 stays 0 in device mode), so only INTSTS0 is checked.
    if (!usb_irq_still_pending()) usbfs_isr_spurious++;

    for (unsigned pass = 0; pass < USB_ISR_MAX_PASSES; pass++) {
        tud_int_handler(0);

        (void) R_USB_FS0->INTSTS0;               // manual: read peripheral once...
        __NOP(); __NOP(); __NOP(); __NOP();      // ...and wait 2 module clocks
        R_BSP_IrqStatusClear((IRQn_Type) 6);

        if (!usb_irq_still_pending()) break;
        usbfs_isr_repass++;

        if (pass == USB_ISR_MAX_PASSES - 1) {
            usbfs_isr_giveups++;
            usbfs_stuck_intsts0 = R_USB_FS0->INTSTS0;
            usbfs_stuck_brdy    = R_USB_FS0->BRDYSTS;
            usbfs_stuck_nrdy    = R_USB_FS0->NRDYSTS;
            usbfs_stuck_bemp    = R_USB_FS0->BEMPSTS;
        }
    }
}

void usb_cli_isr_count(void) {
    print("usbfs_int_isr fired "); printDec((Long) usbfs_isr_count); print(" times");
    print(", re-passes "); printDec((Long) usbfs_isr_repass);
    print(", spurious "); printDec((Long) usbfs_isr_spurious);
    print(", give-ups "); printDec((Long) usbfs_isr_giveups); printCr();
    if (usbfs_isr_giveups) {
        print("  last give-up: INTSTS0="); printHex((Long) usbfs_stuck_intsts0);
        print(" BRDYSTS=");  printHex((Long) usbfs_stuck_brdy);
        print(" NRDYSTS=");  printHex((Long) usbfs_stuck_nrdy);
        print(" BEMPSTS=");  printHex((Long) usbfs_stuck_bemp); printCr();
    }
}

// tusb_init_ok / intenb0_after_init — captured once, right after tusb_init()
// returns, from the SAME boot instance. tusb_init() silently swallows any
// TU_ASSERT/TU_VERIFY failure along its class-driver-init -> dcd_init() ->
// dcd_int_enable() call chain (CFG_TUSB_DEBUG is 0, so nothing gets logged
// either way) — this is the only way to tell, from this build, whether it
// actually reached dcd_init() or bailed out earlier. See Docs/usb-handoff.md.
static bool     tusb_init_ok;
static uint16_t intenb0_after_init;

void usb_cli_init_status(void) {
    print("tusb_init() returned "); print(tusb_init_ok ? "true" : "false"); printCr();
    print("INTENB0 immediately after: "); printHex((Long) intenb0_after_init); printCr();
}

// ── usb_action — TinyUSB task pump in the tea.c action queue ─────────────────
//
// Self-rescheduling: always present in the queue so tud_task() is serviced
// on every pass.  USB interrupts poke the RUSB2 driver; tud_task() drains
// the resulting events and fires DFU callbacks.

// USB is interrupt-driven: usbfs_int_isr() services the peripheral and
// tud_task() (below) dispatches the resulting events.
//
// History (Docs/usb-handoff.md): for a long time the interrupt appeared to
// stop being delivered after the first couple of entries, and a scheduler
// poll of tud_int_handler() was the only thing that kept USB alive. The
// cause was a lost wakeup in the ISR -- USBFS_USBI is the OR of all enabled
// INTSTS0 bits through an edge/level detector (RA4M1 manual, Fig. 27.11), so
// a status bit left set when the ISR returned, or one whose edge was erased
// by the ICU IR clear, hid every later event. The ISR now re-checks after
// clearing IR and loops, and the poll has been removed: it passed an 8 h
// soak, 12 hot-plug cycles and 6 cold power-ons without it.
static void usb_action(void) {
    tud_task();
    later(usb_action);
}

// ── Arduino DFU bootloader entry flag ────────────────────────────────────────
//
// The bootloader's double-tap flag is NOT in SRAM.  On Cortex-M4 RA parts the
// Arduino Renesas bootloader keeps it in the VBATT backup registers:
//
//     R_SYSTEM->VBTBKR[0..3] == 0x07738135  →  enter DFU instead of the app
//
// Confirmed by disassembling the shipped 16 KB bootloader
// (Docs/nano_r4_bootloader_16k.bin), first thing its main() does:
//
//     0020e2  ldr    r3, ← 4001E000        ( R_SYSTEM )
//     0020e4  ldr    r2, ← 07738135        ( magic )
//     0020e6  ldr.w  r1, [r3, #0x500]      ( VBTBKR[0] @ 0x4001E500 )
//     0020ea  cmp    r1, r2
//     0020ec  bne.w  #0x23be               ( no match → normal boot )
//
// Three things are required, and all three matter:
//
//   1. PRCR PRC1 unlocked (0xA502) around the writes, relocked (0xA500) after.
//   2. VBTBER.VBAE (bit 3) set FIRST.  Without it the backup registers accept
//      writes and read back correctly but do not RETAIN across the reset —
//      which is exactly why the previous implementation silently failed: the
//      magic looked fine on read-back and was gone by the time the bootloader
//      looked at it.
//   3. The VBATT domain sits behind a slow clock, so writes are not visible
//      immediately.  Read back and confirm rather than assuming they landed.
//
// The old code wrote the magic to 0x20003FFC, which is unused SRAM and has
// nothing to do with this bootloader.  It never worked.

#define DFU_MAGIC         0x07738135UL

#define DFU_BOOT_FLAG     (*(volatile uint32_t *) &R_SYSTEM->VBTBKR[0])

// Poll the VBATT domain until a write becomes visible.  10 ms total is far
// more than the handful of slow-clock cycles this actually needs.
#define VBATT_SETTLE_TRIES  100
#define VBATT_SETTLE_US     100

// Arm the bootloader's DFU flag.  Returns false if the flag could not be made
// to stick, in which case the caller must NOT reset — a reset without the flag
// just boots the application again and looks like the command did nothing.
// Only VBTBKR[0] is verified. VBTBER is the enable, not the payload, and it
// does not necessarily read back what was written — an earlier version checked
// it too and reported failure on boards where the magic had in fact landed
// correctly. If this ever fails, the reported values say which half went wrong.
static bool dfu_arm_bootloader(void) {
    R_SYSTEM->PRCR = PRCR_PRC1_UNLOCK;

    R_SYSTEM->VBTBER = VBTBER_VBAE;
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);

    DFU_BOOT_FLAG = DFU_MAGIC;
    for (int i = 0; i < VBATT_SETTLE_TRIES; i++) {
        if (DFU_BOOT_FLAG == DFU_MAGIC) break;
        R_BSP_SoftwareDelay(VBATT_SETTLE_US, BSP_DELAY_UNITS_MICROSECONDS);
    }

    uint32_t seen  = DFU_BOOT_FLAG;
    uint32_t enable = R_SYSTEM->VBTBER;

    R_SYSTEM->PRCR = PRCR_LOCK;

    if (seen != DFU_MAGIC) {
        print("dfu: VBTBKR[0] read back "); printDec((Long) seen);
        print(" VBTBER ");                  printDec((Long) enable);
        printCr();
        return false;
    }
    return true;
}

// ── reboot_to_dfu_action — deferred reset into Arduino DFU bootloader ─────────
//
// Runs one scheduler tick after tud_dfu_runtime_reboot_to_dfu_cb() so the
// USB DETACH ACK leaves before we pull the rug out.
//
// Guard: dfu-util sends DFU_DETACH at the end of every upload while it is
// still running, which races with our firmware boot.  We capture the tick
// count at USB init time and refuse any DETACH that arrives within
// DFU_MIN_ELAPSED ticks of that baseline.  Using elapsed time (not absolute
// get_ticks()) is necessary because the DFU bootloader may leave GPT0 running
// with an already-large counter, making an absolute threshold fire immediately.

#define DFU_MIN_ELAPSED (3L * ONE_SECOND)   // 3 s — dfu-util exits well inside this

// get_ticks() is GPT0's raw counter (clocks.c:110) and GPT0 is opened with
// period_counts = 0x7FFFFFFF, so it wraps at 0x80000000 rather than 2^32.
// A plain signed subtraction goes negative across that wrap and the guard then
// rejects everything forever — which is very likely why `dfu` appeared to do
// nothing for so long, on top of the wrong magic address. Do the subtraction
// modulo the counter's actual range.
#define TICK_MODULUS_MASK  0x7FFFFFFFUL

static Long usb_init_tick;

static uint32_t dfu_ticks_since_init(void) {
    return ((uint32_t) get_ticks() - (uint32_t) usb_init_tick) & TICK_MODULUS_MASK;
}

static void reboot_to_dfu_action(void) {
    uint32_t elapsed = dfu_ticks_since_init();
    if (elapsed < (uint32_t) DFU_MIN_ELAPSED) {
        print("dfu: ignored - only "); printDec((Long) elapsed);
        print(" of ");                 printDec((Long) DFU_MIN_ELAPSED);
        print(" ticks since usb init");
        printCr();
        return;
    }

    if (!dfu_arm_bootloader()) {
        print("dfu: could not arm bootloader flag - not resetting"); printCr();
        return;
    }

    print("dfu: entering bootloader..."); printCr();
    NVIC_SystemReset();
}

// ── TinyUSB DFU Runtime callback ──────────────────────────────────────────────

void tud_dfu_runtime_reboot_to_dfu_cb(void) {
    later(reboot_to_dfu_action);
}

// ── usb_hw_reset — put USBFS back to a known state before TinyUSB sees it ────
//
// Yes, USB can be reset in software, and it needs to be here.
//
// The Arduino bootloader's boot5() hands over after calling
// R_BSP_MODULE_STOP(FSP_IP_USBFS) but without detaching from the bus first: it
// never clears SYSCFG.DPRPU, so the D+ pull-up can still be asserted and the
// host still believes the DFU device is present. Arriving from DFU mode is
// therefore a genuinely different starting state than arriving from the
// application, which is exactly the asymmetry we have been chasing.
//
// Sequence, in this order:
//   1. START the module so its registers are writable at all.
//   2. Set SCKE and confirm it reads back 1 — the manual (27.2.1, Note 2)
//      requires this before USBE may be changed at all; skipping it means
//      step 4's USBE clear runs against SCKE in whatever state MSTP
//      module-stop left it (0, the reset default), a documented
//      sequencing violation. Every boot tested so far has been a DFU
//      handoff, where SCKE was already 1 (inherited from the bootloader's
//      own live USB session), which is almost certainly why this hasn't
//      been observed to matter yet — a genuine cold boot is the
//      untested case. Matches what TinyUSB's own dcd_init() and RASC's
//      native r_usb_basic driver (hw_usb_pmodule_init()) both do
//      explicitly. See Docs/usb-handoff.md.
//   3. Drop DPRPU — the host sees a real detach and tears the old device down.
//   4. Mask and clear every USB interrupt source, so nothing is left asserted
//      to re-enter the ISR before tud_task() is running to service it.
//   5. Clear USBE, then STOP the module — on RA this resets the block.
//   6. START it again so TinyUSB inherits reset-default registers.
//
// The delay after the detach matters: hosts need ~10 ms of SE0 to register a
// disconnect. 20 ms was cheap insurance at boot; bumping it to 300 ms made
// zero difference to the DFU-restart storm (confirmed via usb_isr_snapshot()/
// `usbcrash`: DVSQ never advances past Default state, emitq stays empty
// throughout — millions of INTENB0-legitimate interrupts, not one stuck bit,
// not the output() path, and not fixed by more settle time). So it isn't
// buying safety in that scenario — but paying it unconditionally on EVERY
// boot is a separate, real cost: a genuine cold boot has nothing to detach
// from (DPRPU is already low — nothing, bootloader included, left a
// pull-up asserted), so this delay is pure wasted time before we ever
// signal presence to the host. That's a real candidate for why cold boot
// enumeration never even starts: by the time our pull-up finally goes up
// (after boot-time printing/init + this delay + the module stop/restart
// cycle), the host's initial attach-detection window may already have
// closed. Only pay the long delay when there's actually something stale to
// tear down. See Docs/usb-handoff.md.
#define USB_DETACH_SETTLE_MS  300U
#define USB_SYSCFG_USBE   (1U << 0)
#define USB_SYSCFG_DPRPU  (1U << 4)
#define USB_SYSCFG_SCKE   (1U << 10)

static void usb_hw_reset(void) {
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);

    R_USB_FS0->SYSCFG |= USB_SYSCFG_SCKE;
    while (!(R_USB_FS0->SYSCFG & USB_SYSCFG_SCKE)) {}

    bool was_attached = (R_USB_FS0->SYSCFG & USB_SYSCFG_DPRPU) != 0;

    R_USB_FS0->SYSCFG  &= (uint16_t) ~USB_SYSCFG_DPRPU;   // detach from the bus
    R_USB_FS0->INTENB0  = 0;                              // mask all sources
    R_USB_FS0->INTENB1  = 0;
    R_USB_FS0->BRDYENB  = 0;
    R_USB_FS0->INTSTS0  = 0;                              // write 0 to clear
    R_USB_FS0->INTSTS1  = 0;
    R_USB_FS0->SYSCFG  &= (uint16_t) ~USB_SYSCFG_USBE;

    if (was_attached) {
        R_BSP_SoftwareDelay(USB_DETACH_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }

    // This gap is a bare timed delay standing in for a hardware handshake
    // FSP's generic R_BSP_MODULE_STOP/START don't expose for USBFS: they
    // toggle the MSTP bit and return, with no flag confirming the block
    // actually finished resetting internally before the next write. Compare
    // board-Discovery's working STM32 implementation (same TimbreOS
    // scheduler, same Cortex-M4 core): its USB_CoreReset() blocks on
    // GRSTCTL.AHBIDL actually going high -- a true handshake that can't
    // race regardless of CPU speed. We have no equivalent status flag to
    // poll here, so this is a guess at how long that takes -- and it's
    // exactly the kind of gap a debugger's single-stepping papers over for
    // free (real elapsed time between steps is generous) while full-speed
    // execution might not respect reliably, which matches what was
    // observed: the storm/stall is far less likely to reproduce when
    // slowed by breakpoints. 1 ms was a guess with no basis; widening it
    // substantially is the cheapest next experiment. See
    // Docs/usb-handoff.md.
    R_BSP_MODULE_STOP(FSP_IP_USBFS, 0);
    R_BSP_SoftwareDelay(20, BSP_DELAY_UNITS_MILLISECONDS);
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);

    NVIC_DisableIRQ((IRQn_Type) 6);       // USBFS_INT — see header comment
    NVIC_ClearPendingIRQ((IRQn_Type) 6);

    // NVIC_ClearPendingIRQ() only clears the NVIC's own pending latch. The
    // ICU sits underneath it with a SEPARATE per-vector latch (IELSR[6].IR)
    // that drives the NVIC line — confirmed under a live debugger: the
    // storm re-enters usbfs_int_isr() with INTSTS0 AND INTENB0 both 0 (the
    // peripheral has nothing enabled and nothing pending, so it cannot be
    // the one generating these requests), meaning IELSR[6].IR was latched
    // stale from some transient (very plausibly the module STOP/START cycle
    // just above) and never cleared before NVIC_EnableIRQ() went live in
    // tusb_init(), so the CPU re-takes the same already-latched request
    // forever. R_BSP_IrqStatusClear() is the FSP helper every normal
    // R_xxx_Open() call gets automatically via R_BSP_IrqCfgEnable() -- our
    // hand-rolled TinyUSB init never went through that path, so this was
    // never done. See Docs/usb-handoff.md.
    R_BSP_IrqStatusClear((IRQn_Type) 6);
}

// ── usb_transport_init — call once from hal_entry() ──────────────────────────

// USB_TRANSPORT_ENABLE — the TinyUSB device stack is unfinished and takes the
// board down when started, so it is off by default.
//
// With it off, usb_hw_reset() still runs: USBFS is left detached, every
// interrupt source masked and cleared, NVIC entry disabled. That is what makes
// the board boot reliably after a DFU download, where the bootloader hands over
// with the peripheral live and the host still attached.
//
// The `dfu` CLI word is unaffected — it arms VBTBKR[0] and resets, which needs
// no USB stack at all. So the route into the bootloader stays open while the
// device stack is out of service.
//
// Set to 1 when picking the USB work back up. See Docs/usb-handoff.md.
//
// Re-enabled (2026-08-18) under a live J-Link/SWD debug session (see
// Docs/usb-handoff.md) — a real debugger can halt/reset via SWD regardless
// of firmware state, so the storm hanging the board is no longer a recovery
// risk the way it was over plain UART. Do not ship a build with this on
// until the storm is fixed; if testing without a debugger attached, revert
// to 0 and recover a hung board via the bootloader's physical
// double-tap-reset DFU entry.
#define USB_TRANSPORT_ENABLE 1

void usb_transport_init(void) {
    // Invalidate last boot's "first entry" snapshot so a quiet boot (no ISR
    // fire at all this time) reports "no snapshot" instead of stale data
    // from a previous storm. usbfs_isr_count itself is a RAM var and already
    // starts at 0 every boot, so the "last" snapshot (gated on that counter)
    // doesn't need this — only the marker-gated "first" one does.
    R_SYSTEM->PRCR   = PRCR_PRC1_UNLOCK;
    R_SYSTEM->VBTBER = VBTBER_VBAE;
    VBT_WORD(48)     = 0;
    R_SYSTEM->PRCR   = PRCR_LOCK;

    usb_hw_reset();                // must precede tusb_init()
    usb_init_tick = get_ticks();   // baseline for DFU_MIN_ELAPSED guard

#if USB_TRANSPORT_ENABLE
#ifdef WITH_USB_NCM
    // Before tusb_init(): that call connects D+, and the host starts
    // enumerating at once. The network stack should be ready by then, not
    // still being built while the host waits.
    usb_ncm_init();
#endif
    tusb_init_ok = tusb_init();
    intenb0_after_init = R_USB_FS0->INTENB0;

    // TinyUSB's own reference port for this exact board (Arduino UNO R4,
    // same RA4M1 die) sets this right after tusb_init() returns —
    // tinyusb/hw/bsp/ra/family.c's board_init_after_tusb(), gated
    // `#if defined(BOARD_UNO_R4)`. We never called it, and never set this
    // bit any other way. VDCEN enables the transceiver's regulator; without
    // it SYSCFG.DPRPU can read back "set" in software while the actual D+
    // pull-up drive is electrically marginal — consistent with what we saw:
    // VBSTS true but the host never reacting cleanly (cold boot) or reacting
    // to what looks like glitchy repeated resets (DFU-restart storm). See
    // Docs/usb-handoff.md.
    R_USB_FS0->USBMC |= R_USB_FS0_USBMC_VDCEN_Msk;

    // Board patch: RASC's own generated config for this exact peripheral
    // (g_basic0_cfg in ra_gen/hal_data.c, from the never-opened native
    // r_usb_basic stack) sets IRQ priority 12 for the USB vector. Our
    // hand-rolled TinyUSB integration never calls R_BSP_IrqCfgEnable() or
    // NVIC_SetPriority() for it at all, leaving it at the NVIC register's
    // reset value of 0 (highest priority) -- confirmed via a live IPR[6]
    // read during this investigation. Matching RASC's own intended
    // priority. (Tested as a suspect for the wedge described in
    // Docs/usb-handoff.md and ruled out -- the lost wakeup was -- but
    // running USB at the highest NVIC priority was never intentional.)
    NVIC_SetPriority((IRQn_Type) 6, 12);

    later(usb_action);
    namedAction(usb_action);
#else
    (void) usb_action;             // referenced only by the enabled path
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TinyUSB descriptor callbacks
// ═══════════════════════════════════════════════════════════════════════════════

// ── String descriptor indices ─────────────────────────────────────────────────

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_DFU,
    STRID_CDC,
#ifdef WITH_USB_NCM
    STRID_NCM,
    STRID_MAC,
#endif
};

// ── Endpoint addresses ────────────────────────────────────────────────────────
//
// DFU Runtime (interface 0) is control-transfer only, no endpoints of its
// own. CDC (interfaces 1+2) gets a fresh notification + bulk pair; RUSB2
// has 9 non-control pipes available, so there's no need to economize the
// way board-Discovery does (it drops the notification endpoint to make
// room for a second class).

#define EP_CDC_NOTIF    0x81    // EP1 IN  — CDC line-state notifications
#define EP_CDC_OUT      0x02    // EP2 OUT — CDC bulk data
#define EP_CDC_IN       0x82    // EP2 IN  — CDC bulk data
#ifdef WITH_USB_NCM
#define EP_NCM_NOTIF    0x83    // EP3 IN  — NCM link-state notifications
#define EP_NCM_OUT      0x04    // EP4 OUT — NCM bulk data
#define EP_NCM_IN       0x84    // EP4 IN  — NCM bulk data
#endif

// ── Device descriptor ─────────────────────────────────────────────────────────
//
// MISC/COMMON/IAD: required at the device level for CDC's own
// control+data interface pair to be grouped by an Interface Association
// Descriptor (TUD_CDC_DESCRIPTOR emits one automatically) — lets Windows
// and macOS load a single composite driver without ambiguity. DFU Runtime
// stays a single, ungrouped interface alongside it.

static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0xCafe,
#ifdef WITH_USB_NCM
    .idProduct          = 0x4005,   // distinct PID: Nano R4 DFU Runtime + CDC + NCM
#else
    .idProduct          = 0x4004,   // distinct PID: Nano R4 DFU Runtime + CDC
#endif
    .bcdDevice          = 0x0100,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&device_desc;
}

// ── Configuration descriptor ──────────────────────────────────────────────────
//
// 3 interfaces: DFU Runtime (0, no endpoints, control-transfer only), CDC
// control (1) + CDC data (2) — TUD_CDC_DESCRIPTOR emits both from a single
// itfnum plus the IAD grouping them.
//
// Attributes 0x0D:
//   DFU_ATTR_WILL_DETACH       (0x08) — device resets on DETACH without host Reset
//   DFU_ATTR_MANIFESTATION_TOLERANT (0x04) — survives manifestation without USB Reset
//   DFU_ATTR_CAN_DOWNLOAD      (0x01) — bootloader supports firmware download
//
// xfer_size 4096 is advisory (the bootloader owns actual transfer sizing).

#ifdef WITH_USB_NCM
// env:nano_r4_web adds an NCM function (interfaces 3+4) for the web page.
#define NUM_INTERFACES      5
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_DFU_RT_DESC_LEN + TUD_CDC_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
#else
#define NUM_INTERFACES      3
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_DFU_RT_DESC_LEN + TUD_CDC_DESC_LEN)
#endif

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, NUM_INTERFACES, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_DFU_RT_DESCRIPTOR(0, STRID_DFU, 0x0D, 1000, 4096),
    TUD_CDC_DESCRIPTOR(1, STRID_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
#ifdef WITH_USB_NCM
    // Last argument: Apple's workaround for macOS dropping the data interface (alt 0) right
    // after enabling it (Apple Developer Forums thread 824727): declare SetEthernetPacketFilter
    // support. TinyUSB handles the request (tud_network_set_packet_filter_cb, weak).
    TUD_CDC_NCM_DESCRIPTOR(3, STRID_NCM, STRID_MAC, EP_NCM_NOTIF, 64, EP_NCM_OUT, EP_NCM_IN,
                           64, CFG_TUD_NET_MTU, 50, NCM_NETWORK_CAPS_ETH_FILTER),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

// ── String descriptors ────────────────────────────────────────────────────────

static const char *const string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   // 0: language — English (0x0409)
    "TimbreWorks",                   // 1: manufacturer
    "ActiveRobot",                   // 2: product
    "",                              // 3: serial — built from the chip's unique ID, see usb_serial_string()
    "TimbreOS DFU",                  // 4: DFU Runtime interface
    "TimbreOS CDC",                  // 5: CDC interface
#ifdef WITH_USB_NCM
    "TimbreOS NCM",                  // 6: NCM interface
    "",                              // 7: MAC address — built from the chip's unique ID, see usb_ncm_mac_string()
#endif
};

// The serial number is the board's name, see board_id.c.
static const char *usb_serial_string(void) {
    return board_serial();
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    static uint16_t desc_str[34];
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        const char *str = (index == STRID_SERIAL) ? usb_serial_string()
#ifdef WITH_USB_NCM
                        : (index == STRID_MAC)    ? usb_ncm_mac_string()
#endif
                                                  : string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 33) chr_count = 33;
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}
