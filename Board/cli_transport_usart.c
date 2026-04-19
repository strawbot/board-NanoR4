// cli_transport_usart.c — SCI2 / RS232 transport for TimbreOS CLI (RA4M1)
//
// Uses the RASC-configured g_uart2 instance on SCI2 (P301 RX, P302 TX).
//
// MVP model — simple, callback-driven, one byte at a time:
//   RX:  UART_EVENT_RX_CHAR pushes bytes into cliq; a tea action drains them
//        into keyIn(). RX is always armed — the SCI driver reports each byte
//        as it arrives via the p_args->data field.
//   TX:  usart_emit() pulls one byte from emitq and calls R_SCI_UART_Write;
//        UART_EVENT_TX_COMPLETE kicks the next byte. Simple and bounded —
//        the DTC link provided by RASC (g_transfer1) makes single-byte writes
//        efficient enough for a CLI.
//
// DMAC-based ring-buffer RX (g_transfer0) is deliberately not used yet; the
// hook point is preserved for later when higher throughput is needed.

#include <stdint.h>
#include <stdbool.h>

#include "hal_data.h"

#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "printers.h"

#include "cli_transport_usart.h"

static void usart_rx_action(void);
static void usart_emit(void);

// ── CLI input queue — filled by cli_rxtx callback, consumed by usart_rx_action
static BQUEUE(100, cliq);

// ── TX single-byte shuttle — held in flight during R_SCI_UART_Write ─────────
static volatile bool   tx_busy;
static          uint8_t tx_byte;

// ── FSP UART callback (wired in RASC as `cli_rxtx`) ─────────────────────────
//
// Called from the SCI ISR on every received character and whenever a Write
// completes. Keep this function short and non-blocking.
void cli_rxtx(uart_callback_args_t * p_args) {
    switch (p_args->event) {

    case UART_EVENT_RX_CHAR: {
        bool was_empty = (qbq(cliq) == 0);
        pushbq((uint8_t)p_args->data, cliq);
        if (was_empty) later(usart_rx_action);
        break;
    }

    case UART_EVENT_TX_COMPLETE:
        tx_busy = false;
        // Chain straight into the next byte — usart_emit is a no-op if emitq
        // is empty.
        usart_emit();
        break;

    default:
        // parity / framing / overrun — ignore for now
        break;
    }
}

// ── EmitEvent target — kicks off one byte of interrupt-driven TX ────────────
static void usart_emit(void) {
    if (tx_busy)     return;
    if (!qbq(emitq)) return;
    tx_byte = (uint8_t)pullbq(emitq);
    tx_busy = true;
    if (R_SCI_UART_Write(&g_uart2_ctrl, &tx_byte, 1) != FSP_SUCCESS) {
        // Write kickoff failed — release the shuttle so a later emit can retry.
        tx_busy = false;
    }
}

// ── tea action: drain cliq into keyIn ──────────────────────────────────────
static void usart_rx_action(void) {
    when(EmitEvent, usart_emit);
    autoEchoOff();

    while (qbq(cliq)) keyIn(pullbq(cliq));
    safe( if (qbq(cliq)) later(usart_rx_action); )
}

// ── usart_transport_init — call once from hal_entry ─────────────────────────
//
// Opens g_uart2 (SCI2). RASC pre-wires pin mux (P301/P302), baud, and the
// cli_rxtx callback. p_transfer_tx = DTC (g_transfer1) is linked in the cfg
// so TX handoff is efficient.
void usart_transport_init(void) {
    R_SCI_UART_Open(&g_uart2_ctrl, &g_uart2_cfg);

    when(EmitEvent, usart_emit);
    autoEchoOff();
}
