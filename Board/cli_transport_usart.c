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

#include "cli_transport.h"
#include "cli_transport_usart.h"

static void usart_rx_action(void);
static void usart_emit(void);

static const cli_transport_hw_t usart_hw = { .emit = usart_emit };

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
//
// RACE: this runs in BOTH contexts — thread context via the EmitEvent hook, and
// ISR context via cli_rxtx() on UART_EVENT_TX_COMPLETE. The old code did
//
//     if (tx_busy) return;  ...  tx_busy = true;
//
// with the test and the set separated by a queue pull. If TX_COMPLETE lands in
// that window both paths claim the shuttle, both call R_SCI_UART_Write, and the
// second one fails. The failure path then cleared tx_busy while a write was
// genuinely in flight — or, in the other interleaving, tx_busy was left true
// with no write pending at all.
//
// The second case is unrecoverable: the TX chain only re-arms from
// TX_COMPLETE, so once it stops with tx_busy stuck true, emitq never drains
// again and the next print() blocks forever.
//
// Fix: claim the shuttle atomically. Everything else is unchanged.
//
// Remaining (much narrower) failure mode: if R_SCI_UART_Write succeeds but
// TX_COMPLETE never arrives, tx_busy stays true forever. If that ever shows up,
// add a timeout in the scheduler that re-arms when emitq is non-empty and the
// SCI has been idle.
static void usart_emit(void) {
    FSP_CRITICAL_SECTION_DEFINE;

    bool claimed = false;

    FSP_CRITICAL_SECTION_ENTER;
    if (!tx_busy && qbq(emitq)) {
        tx_byte = (uint8_t) pullbq(emitq);
        tx_busy = true;
        claimed = true;
    }
    FSP_CRITICAL_SECTION_EXIT;

    if (!claimed) return;

    if (R_SCI_UART_Write(&g_uart2_ctrl, &tx_byte, 1) != FSP_SUCCESS) {
        // Write kickoff failed — release the shuttle so a later emit can retry.
        tx_busy = false;
    }
}

// ── usart_drain — pace the producer so emitq never fills ────────────────────
//
// The TX chain is self-sustaining only while bytes keep completing: each
// TX_COMPLETE calls usart_emit() for the next byte. When emitq runs empty the
// chain stops, and restarting it needs an explicit usart_emit(), which normally
// arrives via when(EmitEvent, usart_emit).
//
// output() blocks when emitq fills and runs actions while it waits — but
// running actions does not signal EmitEvent. So if the chain went idle at the
// wrong moment, nothing ever restarts it and the wait never ends. The board
// stops part-way through a string with no fault and no console.
//
// Boot printing is where this bites: nothing calls run() yet, and the CPU
// queues characters thousands of times faster than 115200 baud drains them.
//
// Calling this between boot prints keeps the queue shallow and, crucially,
// calls usart_emit() directly so an idle chain is re-armed rather than waited
// on. Interrupts stay enabled throughout.
//
// This is containment. The cure is for output() to kick the transport itself
// while blocked — see Docs/output-backpressure-design.md — after which this
// function can be deleted.
void usart_drain(void) {
    uint32_t guard = 4000000UL;   // bounded — never trade one hang for another
    while (qbq(emitq) && guard--) {
        usart_emit();
    }
}

// ── tea action: drain cliq into keyIn ──────────────────────────────────────
static void usart_rx_action(void) {
    while (qbq(cliq)) cli_transport_rx_byte(&usart_hw, (uint8_t) pullbq(cliq));
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
    namedAction(usart_rx_action);
}
