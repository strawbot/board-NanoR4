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
// Phase 2 (current): CDC ACM for the CLI, alongside DFU Runtime.
// Phase 3: add CFG_TUD_NCM 1 for composite CDC+ECM.

#define CFG_TUD_DFU_RUNTIME         1       // DFU Runtime — host can trigger bootloader entry
#define CFG_TUD_DFU                 0

#define CFG_TUD_CDC                 1       // CDC ACM for the CLI, mirrors the UART transport
#define CFG_TUD_CDC_RX_BUFSIZE      256     // RX FIFO — characters from host
#define CFG_TUD_CDC_TX_BUFSIZE      256     // TX FIFO — characters to host
#define CFG_TUD_CDC_EP_BUFSIZE      64      // USB packet size

#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_USBTMC              0
#define CFG_TUD_BTH                 0
#define CFG_TUD_ECM_RNDIS           0

// ── IP over USB (CDC-NCM) — only in env:nano_r4_web ──────────────────────────
//
// Frame size = IP MTU 576 + 14-byte Ethernet header (see Board/lwip/lwipopts.h).
// A transmit NTB holds one frame plus its headers; two of them are the
// minimum for TCP (with one, a second segment sent while the first is still
// on the wire is refused). The receive NTB is larger because the host may
// pack several frames into one transfer.
#ifdef WITH_USB_NCM
#define CFG_TUD_NCM                         1
#define CFG_TUD_NET_MTU                     590
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE         700
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE        1200
#define CFG_TUD_NCM_IN_NTB_N                2
#define CFG_TUD_NCM_OUT_NTB_N               1
#define CFG_TUD_NCM_IN_MAX_DATAGRAMS_PER_NTB    1
#define CFG_TUD_NCM_OUT_MAX_DATAGRAMS_PER_NTB   2
// Apple's workaround for macOS dropping the data interface (alt 0) right after enabling it
// (Apple Developer Forums thread 824727): declare SetEthernetPacketFilter support. The class
// driver does not implement it; macOS tolerates the request failing.
#define CFG_TUD_NCM_NETWORK_CAPABILITIES    0x01
#else
#define CFG_TUD_NCM                         0
#endif

#endif // TUSB_CONFIG_H
