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

// ── reboot_to_dfu_action — deferred reset into Arduino DFU bootloader ─────────
//
// Runs one scheduler tick after tud_dfu_runtime_reboot_to_dfu_cb() so the
// USB DETACH ACK leaves before we pull the rug out.
//
// The magic value 0x07738135 written to the last 4 bytes of SRAM (0x20003FFC)
// is checked by the Arduino UNO R4 Minima bootloader on startup: if it matches,
// the bootloader enters DFU mode instead of launching user code.
//
// Guard: dfu-util sends DFU_DETACH at the end of every upload while it is
// still running, which races with our firmware boot.  We capture the tick
// count at USB init time and refuse any DETACH that arrives within
// DFU_MIN_ELAPSED ticks of that baseline.  Using elapsed time (not absolute
// get_ticks()) is necessary because the DFU bootloader may leave GPT0 running
// with an already-large counter, making an absolute threshold fire immediately.

#define DFU_MIN_ELAPSED (3L * ONE_SECOND)   // 3 s — dfu-util exits well inside this

static Long usb_init_tick;

static void reboot_to_dfu_action(void) {
    if ((get_ticks() - usb_init_tick) < DFU_MIN_ELAPSED) return;
    *((volatile uint32_t *)0x20003FFCUL) = 0x07738135UL;
    NVIC_SystemReset();
}

// ── TinyUSB DFU Runtime callback ──────────────────────────────────────────────

void tud_dfu_runtime_reboot_to_dfu_cb(void) {
    later(reboot_to_dfu_action);
}

// ── usb_transport_init — call once from hal_entry() ──────────────────────────

void usb_transport_init(void) {
    usb_init_tick = get_ticks();   // baseline for DFU_MIN_ELAPSED guard
    tusb_init();
    later(usb_action);
    namedAction(usb_action);
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
