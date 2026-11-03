// Board/edge_test.c — plain-GPIO edge-count wire test for the limb-bus
// link (D11/D12).
//
// Bypasses SCI9 framing entirely: reconfigures D11 (P109) as a free-running
// GPIO output and toggles it a fixed number of times at a chosen rate, while
// D12 (P110) is reconfigured as a GPIO input with ICU IRQ3 (its only external
// IRQ option per the Arduino core's own pin table) counting every edge. A
// matching test on the Nucleo substitute node (limb_board/bench_bringup/
// sma_node_substitute, stage4_edge_test) does the same on its side of the
// wire. Comparing the TX count against the RX count at increasing rates
// finds the wire's actual bandwidth margin before trusting any UART framing
// on top of it. Same compile-always/`#ifdef`-guarded-body structure as
// limb_uart.c and usb_native_test.c -- see those files.
//
// No r_icu driver is vendored into this project (only the drivers
// limb_uart.c etc. already needed are, see board-NanoR4/ra/fsp/src/), so
// this pokes the ICU's IRQCR register directly rather than adding a new
// driver module just for a test tool. R_BSP_IrqCfgEnable (bsp_irq.h) is
// generic across all peripherals and already used by hand for GPT7/SCI9's
// vectors, so it's reused here for IRQ3's NVIC wiring.

#include "edge_test.h"

#ifdef LIMB_UART_DEV

#include "hal_data.h"
#include "r_ioport.h"
#include "vector_data.h"

#include "tea.h" // sysTicks() -- 48 MHz DWT cycle counter, project_defs.h
#include "cli.h"
#include "printers.h"

#define EDGE_TX_PIN BSP_IO_PORT_01_PIN_09 // D11, P109
#define EDGE_RX_PIN BSP_IO_PORT_01_PIN_10 // D12, P110, ICU IRQ3

static volatile uint32_t g_edge_count;

void edge_test_irq3_isr(void) {
    R_BSP_IrqStatusClear(VECTOR_NUMBER_ICU_IRQ3);
    g_edge_count++;
}

void edge_test_cli_rx_arm(void) {
    R_IOPORT_PinCfg(&g_ioport_ctrl, EDGE_RX_PIN,
        (uint32_t)IOPORT_CFG_PORT_DIRECTION_INPUT | (uint32_t)IOPORT_CFG_IRQ_ENABLE);
    R_ICU->IRQCR[3] = 0x02U; // IRQMD = both edges, no digital filter (see r_external_irq_api.h)
    g_edge_count = 0U;
    R_BSP_IrqStatusClear(VECTOR_NUMBER_ICU_IRQ3);
    R_BSP_IrqCfgEnable(VECTOR_NUMBER_ICU_IRQ3, 12U, NULL);
    print("edge-rx: armed\n");
}

void edge_test_cli_rx_clr(void) {
    g_edge_count = 0U;
    print("edge-rx: cleared\n");
}

void edge_test_cli_rx_read(void) {
    lit((Cell)g_edge_count);
}

// count half-us -- toggle D11 `count` times, `half-us` microseconds between
// each transition (period = 2*half-us). Busy-waits for the whole burst --
// a bench test tool, not Robot/ code, same convention as the rest of this
// file's bit-banging.
void edge_test_cli_tx(void) {
    Cell half_us = ret();
    Cell count = ret();

    R_IOPORT_PinCfg(&g_ioport_ctrl, EDGE_TX_PIN, (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    bsp_io_level_t lv = BSP_IO_LEVEL_LOW;
    R_IOPORT_PinWrite(&g_ioport_ctrl, EDGE_TX_PIN, lv);

    uint32_t cycles_per_half = 48U * (uint32_t)half_us;
    for (uint32_t i = 0; i < (uint32_t)count; i++) {
        lv = (lv == BSP_IO_LEVEL_LOW) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;
        uint32_t start = (uint32_t)sysTicks();
        while (((uint32_t)sysTicks() - start) < cycles_per_half) {
        }
        R_IOPORT_PinWrite(&g_ioport_ctrl, EDGE_TX_PIN, lv);
    }
    print("edge-tx: done count=");
    printDec0((unsigned)count);
    printCr();
}

#else // !LIMB_UART_DEV

#include "cli.h"
#include "printers.h"

void edge_test_irq3_isr(void) {}

void edge_test_cli_tx(void) {
    (void)ret();
    (void)ret();
    print("edge_test: not built in this firmware (needs env:nano_r4_limb)\n");
}

void edge_test_cli_rx_arm(void) {
    print("edge_test: not built in this firmware (needs env:nano_r4_limb)\n");
}

void edge_test_cli_rx_clr(void) {
    print("edge_test: not built in this firmware (needs env:nano_r4_limb)\n");
}

void edge_test_cli_rx_read(void) {
    lit(0);
    print("edge_test: not built in this firmware (needs env:nano_r4_limb)\n");
}

#endif // LIMB_UART_DEV
