// Board/limb_uart.h — Nano R4 pin/peripheral shim for Robot/io/limb_bus.

#ifndef BOARD_LIMB_UART_H
#define BOARD_LIMB_UART_H

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
