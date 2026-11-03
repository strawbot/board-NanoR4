// Board/edge_test.h — plain-GPIO edge-count wire test for the limb-bus
// link (D11/D12), independent of SCI9 framing. See edge_test.c.

#ifndef BOARD_EDGE_TEST_H
#define BOARD_EDGE_TEST_H

void edge_test_cli_tx(void);      // (count half-us) toggle D11 `count` times, `half-us` apart
void edge_test_cli_rx_arm(void);  // ( -- ) arm D12 as an edge-counted IRQ input, counter reset to 0
void edge_test_cli_rx_clr(void);  // ( -- ) reset the edge counter to 0 without disarming
void edge_test_cli_rx_read(void); // ( -- count ) read the edge counter

// ISR for ra_gen/vector_data.c slot 21 (ICU IRQ3).
void edge_test_irq3_isr(void);

#endif // BOARD_EDGE_TEST_H
