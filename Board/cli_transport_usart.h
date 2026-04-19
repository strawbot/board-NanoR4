#ifndef CLI_TRANSPORT_USART_H
#define CLI_TRANSPORT_USART_H

// cli_transport_usart.h — SCI2 / RS232 transport for TimbreOS CLI (RA4M1).
//
// RASC configures SCI2 (g_uart2) on P301/P302, baud, and the cli_rxtx callback.
// All IRQ routing is handled by the FSP SCI driver — no manual IRQ handler
// wiring is required from board init code.

// Call once from hal_entry after g_hal_init(). Opens g_uart2 and registers
// usart_emit as the EmitEvent target so printers can deliver TX bytes.
void usart_transport_init(void);

#endif // CLI_TRANSPORT_USART_H
