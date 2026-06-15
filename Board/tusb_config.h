#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// ── Board / controller ────────────────────────────────────────────────────────
//
// Hardware prerequisites (configured in RASC, NOT here):
//   USBFS peripheral enabled; USB_DM (PA11) / USB_DP (PA12) configured for USB.
//   USBFS_INT event wired to IRQ 6 in ra_gen/vector_data.h/.c.
//   The ISR stub (usbfs_int_isr) lives in usb_init.c and calls tud_int_handler(0).
//
// Clock: RA4M1 internal PLL supplies 48 MHz to the USBFS peripheral; no manual
// PLLQ adjustment is required (FSP / bsp_clocks handles it).

#define CFG_TUSB_MCU                OPT_MCU_RAXXX       // Renesas RA family (RUSB2 driver)
#define CFG_TUSB_OS                 OPT_OS_NONE          // no RTOS — tea.c handles scheduling
#define CFG_TUSB_DEBUG              0

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// ── Memory ────────────────────────────────────────────────────────────────────

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

// ── Device stack ──────────────────────────────────────────────────────────────

#define CFG_TUD_ENDPOINT0_SIZE      64

// ── Class drivers ─────────────────────────────────────────────────────────────
//
// Phase 1: DFU Runtime only.
// Phase 2: add CFG_TUD_CDC 1 and CFG_TUD_NCM 1 for composite CDC+ECM.

#define CFG_TUD_DFU_RUNTIME         1       // DFU Runtime — host can trigger bootloader entry
#define CFG_TUD_DFU                 0

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_USBTMC              0
#define CFG_TUD_BTH                 0
#define CFG_TUD_ECM_RNDIS           0
#define CFG_TUD_NCM                 0

#endif // TUSB_CONFIG_H
