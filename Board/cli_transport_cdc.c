// Board/cli_transport_cdc.c — USB CDC-ACM transport for TimbreOS CLI
//
// Lives in the board, not Robot/, because it talks to TinyUSB (tusb.h): a
// specific USB medium driver, which Robot/CLAUDE.md keeps out of Robot/.
// It is generic across boards that use TinyUSB, though: it needs only
// tud_cdc_* and a CDC interface in the board's own USB descriptors
// (Board/usb_init.c here). The board-agnostic part -- the transport bridge
// and the "last transport to receive input is the active output" rule --
// is Robot/cli/cli_transport.[ch]. No board registers or pins here.
//
// TinyUSB drives the USB stack from whichever board action pumps
// tud_task() (see e.g. board-NanoR4/Board/usb_init.c's usb_action).
// cdc_rx_action is posted by TinyUSB's CDC RX callback and drains the CDC
// RX FIFO through the shared cli_transport_rx_byte() bridge, which claims
// EmitEvent for this transport before each byte.
//
// TX backpressure: cdc_emit checks tud_cdc_write_available() before
// pulling from emitq so no byte is ever lost. When the TinyUSB TX FIFO is
// full, cdc_emit queues cdc_emit_action exactly once (guarded by
// emit_retry_pending) and returns; the board's own tud_task() pump runs
// ahead of it in the action queue, flushes the FIFO to USB, and
// tud_cdc_tx_complete_cb resumes the drain from there.

#include <stdint.h>
#include <stdbool.h>

#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "tusb.h"

#include "cli_transport.h"

static void cdc_emit(void);
static const cli_transport_hw_t cdc_hw = { .emit = cdc_emit, .name = "cdc" };

void cdc_rx_action(void);          // posted by tud_cdc_rx_cb
void cdc_emit_action(void);        // non-static — must be addressable by later()

// ── TX retry gate ─────────────────────────────────────────────────────────────
//
// Cleared by cdc_emit_action when it runs. Prevents cdc_emit from pushing
// more than one pending retry into the action queue regardless of how many
// times EmitEvent fires while the TinyUSB TX FIFO is backed up.
static bool emit_retry_pending = false;

// ── emit — drains emitq to USB CDC TX ────────────────────────────────────────

static void cdc_emit(void) {
    if (!cli_transport_is_active(&cdc_hw)) return;     // another transport owns the output
    while (qbq(emitq)) {
        if (tud_cdc_write_available() == 0) {
            // TX FIFO full — flush what is staged and yield. Queue a
            // single retry; the board's tud_task() pump runs first and
            // drains the FIFO to USB before cdc_emit_action resumes here.
            tud_cdc_write_flush();
            if (!emit_retry_pending) {
                emit_retry_pending = true;
                later(cdc_emit_action);
            }
            return;
        }
        uint8_t ch = pullbq(emitq);
        tud_cdc_write(&ch, 1);
    }
    tud_cdc_write_flush();
}

void cdc_emit_action(void) {
    emit_retry_pending = false;
    cdc_emit();
}

// ── cdc_rx_action — posted by TinyUSB CDC RX callback ────────────────────────

void cdc_rx_action(void) {
    if (!tud_cdc_connected()) return;
    if (!tud_cdc_available()) return;

    uint8_t buf[64];
    uint32_t count = tud_cdc_read(buf, sizeof(buf));

    for (uint32_t i = 0; i < count; i++) {
        cli_transport_rx_byte(&cdc_hw, buf[i]);
    }

    // If more data arrived while we were processing, requeue.
    if (tud_cdc_available()) {
        later(cdc_rx_action);
    }
}

// ── TinyUSB CDC callbacks — called from tud_task() ───────────────────────────

// Posted when RX data is available.
void tud_cdc_rx_cb(uint8_t itf) {
    (void) itf;
    later(cdc_rx_action);
}

// TX FIFO drained to USB — if we have more to send, resume the drain.
void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void) itf;
    if (cli_transport_is_active(&cdc_hw) && qbq(emitq) && !emit_retry_pending) {
        emit_retry_pending = true;
        later(cdc_emit_action);
    }
}

// Connection state change — print a prompt on connect. No received byte to
// hand to cli_transport_rx_byte() here, so use cli_transport_claim()
// directly — this is also what makes `show-cli` reflect a CDC connection
// immediately rather than only after the first keystroke.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void) itf; (void) rts;
    if (dtr) {
        cli_transport_claim(&cdc_hw);
        autoEchoOff();          // terminal emulator handles local echo on CDC
        dotPrompt();
    } else {
        cli_transport_release(&cdc_hw);     // terminal closed: nobody is reading CDC output
    }
}

// Cable pulled or bus reset: same.
void tud_umount_cb(void) {
    cli_transport_release(&cdc_hw);
}
