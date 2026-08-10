// usb_init.c — TinyUSB task pump and DFU Runtime descriptor for Nano R4
//
// Hardware prerequisites (configured in RASC, NOT here):
//   USBFS peripheral; USB_DM on PA11, USB_DP on PA12.
//   USBFS_INT event wired to IRQ 6 in ra_gen/vector_data.h/.c.
//
// Composite evolution — this file is Phase 1 (DFU Runtime only).
// When CDC + CDC-ECM are added in Phase 2:
//   - Set CFG_TUD_CDC 1 and CFG_TUD_NCM 1 in tusb_config.h.
//   - Extend the configuration descriptor to include the extra interfaces.
//   - Add cli_transport_cdc.c (port from board-Discovery) for CLI over USB.
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

#include "tusb.h"
#include "usb_init.h"

// ── USBFS interrupt handler ───────────────────────────────────────────────────
//
// Declared in ra_gen/vector_data.h and registered in the g_vector_table at
// slot 6 (USBFS_INT_IRQn).  The FSP ICU routes ELC_EVENT_USBFS_INT here.

void usbfs_int_isr(void) {
    tud_int_handler(0);
}

// ── usb_action — TinyUSB task pump in the tea.c action queue ─────────────────
//
// Self-rescheduling: always present in the queue so tud_task() is serviced
// on every pass.  USB interrupts poke the RUSB2 driver; tud_task() drains
// the resulting events and fires DFU callbacks.

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
#define PRCR_PRC1_UNLOCK  0xA502U
#define PRCR_LOCK         0xA500U
#define VBTBER_VBAE       (1U << 3)

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
//   2. Drop DPRPU — the host sees a real detach and tears the old device down.
//   3. Mask and clear every USB interrupt source, so nothing is left asserted
//      to re-enter the ISR before tud_task() is running to service it.
//   4. Clear USBE, then STOP the module — on RA this resets the block.
//   5. START it again so TinyUSB inherits reset-default registers.
//
// The delay after the detach matters: hosts need ~10 ms of SE0 to register a
// disconnect. 20 ms is cheap insurance at boot.
#define USB_SYSCFG_USBE   (1U << 0)
#define USB_SYSCFG_DPRPU  (1U << 4)

static void usb_hw_reset(void) {
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);

    R_USB_FS0->SYSCFG  &= (uint16_t) ~USB_SYSCFG_DPRPU;   // detach from the bus
    R_USB_FS0->INTENB0  = 0;                              // mask all sources
    R_USB_FS0->INTENB1  = 0;
    R_USB_FS0->BRDYENB  = 0;
    R_USB_FS0->INTSTS0  = 0;                              // write 0 to clear
    R_USB_FS0->INTSTS1  = 0;
    R_USB_FS0->SYSCFG  &= (uint16_t) ~USB_SYSCFG_USBE;

    R_BSP_SoftwareDelay(20, BSP_DELAY_UNITS_MILLISECONDS);

    R_BSP_MODULE_STOP(FSP_IP_USBFS, 0);
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);

    NVIC_DisableIRQ((IRQn_Type) 6);       // USBFS_INT — see header comment
    NVIC_ClearPendingIRQ((IRQn_Type) 6);
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
#define USB_TRANSPORT_ENABLE 0

void usb_transport_init(void) {
    usb_hw_reset();                // must precede tusb_init()
    usb_init_tick = get_ticks();   // baseline for DFU_MIN_ELAPSED guard

#if USB_TRANSPORT_ENABLE
    tusb_init();
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
};

// ── Device descriptor ─────────────────────────────────────────────────────────
//
// Class 0/0/0: class defined at interface level.  Appropriate for a
// DFU-Runtime-only device and leaves room for IAD when CDC is added later.

static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0xCafe,
    .idProduct          = 0x4003,   // distinct PID: Nano R4 DFU Runtime
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
// 1 interface: DFU Runtime (no endpoints, control-transfer only)
//
// Attributes 0x0D:
//   DFU_ATTR_WILL_DETACH       (0x08) — device resets on DETACH without host Reset
//   DFU_ATTR_MANIFESTATION_TOLERANT (0x04) — survives manifestation without USB Reset
//   DFU_ATTR_CAN_DOWNLOAD      (0x01) — bootloader supports firmware download
//
// xfer_size 4096 is advisory (the bootloader owns actual transfer sizing).

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_DFU_RT_DESC_LEN)

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_DFU_RT_DESCRIPTOR(0, STRID_DFU, 0x0D, 1000, 4096),
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
    "ActiveRobot",                   // 3: serial → /dev/cu.usbmodemActiveRobot1 on macOS
    "TimbreOS DFU",                  // 4: DFU Runtime interface
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    static uint16_t desc_str[32];
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}
