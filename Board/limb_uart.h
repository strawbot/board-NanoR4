// Board/limb_uart.h — Nano R4 pin/peripheral shim for Robot/io/limb_bus.

#ifndef BOARD_LIMB_UART_H
#define BOARD_LIMB_UART_H

#include "tea.h" // Event

// Fires once the most recent send's bytes have all actually gone out on
// the wire -- not an ack, just "the TX buffer is free to reuse." See
// limb_uart.c's own comment and limb_fw_test.c's pipelined-chunk CLI
// word, the reason this exists. Always safe to call when()/never() on;
// limb_uart_init() leaves it at no_action by default so an unrelated
// send (e.g. a plain limb-ping) firing it is harmless.
extern Event limb_uart_tx_complete_event;

// limb_uart_init — reconfigure D11/D12 (P109/P110) for SCI9, open the UART
// at the limb-bus protocol's 250000 baud, and bind it to limb_bus. Call
// from hal_entry, after g_hal_init(). D11 must not also be claimed by
// sr595_hw in this build -- see platformio.ini's env:nano_r4_limb.
void limb_uart_init(void);

// limb_uart_cli_status — CLI word: print the stored R_SCI_UART_BaudCalculate
// / R_SCI_UART_Open error codes (0 = FSP_SUCCESS) from the last
// limb_uart_init() call.
void limb_uart_cli_status(void);

#endif // BOARD_LIMB_UART_H
