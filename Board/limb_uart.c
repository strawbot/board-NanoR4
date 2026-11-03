// Board/limb_uart.c — SCI9 as the limb-bus controller UART.
//
// Compiles in every environment (not excluded from nano_r4's
// build_src_filter, so limb_bus's CLI word always resolves), but the
// body that actually opens SCI9 is behind LIMB_UART_DEV (only defined
// for env:nano_r4_limb) -- in env:nano_r4 this is an inert stub,
// referencing nothing from hal_data.h/r_sci_uart.h (not even included
// there), so this file can never be what breaks the working firmware
// build. Same structure as usb_native_test.c's USE_NATIVE_USB_STACK
// split; see that file.
//
//   Nano D11  TX  (P109, PSEL=SCI-odd per architecture SS15.4)
//   Nano D12  RX  (P110, PSEL=SCI-odd)
//
// No RASC instance exists for SCI9 in this project (only SCI2, for the
// CLI) -- this driver hand-builds its uart_cfg_t/baud_setting_t the same
// way Board/frame_timer.c hand-builds GPT7's timer_cfg_t, and
// ra_gen/vector_data.c's slots 17-20 (added by hand, same file) point
// SCI9's interrupts at FSP's own generic SCI UART ISRs -- the same ones
// SCI2 already uses at slots 0-3, see Board/cli_transport_usart.c.
//
// D11 = P109 is also sr595_hw's SER line (port-power shift register) in
// the normal firmware. env:nano_r4_limb skips sr595_hw_init() in
// hal_entry.c instead of resolving that pin conflict for real -- see
// limb_board/bench_bringup/BRINGUP_PLAN.md's "Hardware finding" and the
// architecture doc, both still open on which pins the real board uses.

#include "limb_uart.h"

#ifdef LIMB_UART_DEV

#include "hal_data.h"
#include "r_ioport.h"
#include "r_sci_uart.h"
#include "vector_data.h"

#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "printers.h"
#include "limb_bus.h"

#define LIMB_UART_BAUD 250000U // architecture SS2/SS15.1 downstream rate

#define LIMB_TX_PIN BSP_IO_PORT_01_PIN_09 // D11, P109
#define LIMB_RX_PIN BSP_IO_PORT_01_PIN_10 // D12, P110

static sci_uart_instance_ctrl_t g_limb_uart_ctrl;
static baud_setting_t g_limb_baud_setting; // filled by R_SCI_UART_BaudCalculate() before Open()
static fsp_err_t g_limb_baud_err, g_limb_open_err; // queryable via limb_uart_cli_status

static const sci_uart_extended_cfg_t g_limb_cfg_extend = {
    .clock = SCI_UART_CLOCK_INT,
    .rx_edge_start = SCI_UART_START_BIT_FALLING_EDGE,
    .noise_cancel = SCI_UART_NOISE_CANCELLATION_DISABLE,
    .rx_fifo_trigger = SCI_UART_RX_FIFO_TRIGGER_1,
    .p_baud_setting = &g_limb_baud_setting,
    .flow_control = SCI_UART_FLOW_CONTROL_RTS, // unused -- flow_control_pin below is the invalid sentinel, same as g_uart2_cfg_extend
    .flow_control_pin = (bsp_io_port_pin_t)UINT16_MAX,
    .rs485_setting = {
        .enable = SCI_UART_RS485_DISABLE,
        .polarity = SCI_UART_RS485_DE_POLARITY_HIGH,
        .de_control_pin = (bsp_io_port_pin_t)UINT16_MAX,
    },
    .irda_setting = {
        .ircr_bits_b.ire = 0,
        .ircr_bits_b.irrxinv = 0,
        .ircr_bits_b.irtxinv = 0,
    },
};

static void limb_uart_callback(uart_callback_args_t *p_args);

// Same priority as SCI2/everything else in this project (frame_timer.c's
// GPT7 is the one exception, at 1/3 -- nothing here is that time-critical).
static const uart_cfg_t g_limb_cfg = {
    .channel = 9,
    .data_bits = UART_DATA_BITS_8,
    .parity = UART_PARITY_OFF,
    .stop_bits = UART_STOP_BITS_1,
    .p_callback = limb_uart_callback,
    .p_context = NULL,
    .p_extend = &g_limb_cfg_extend,
    .p_transfer_tx = NULL,
    .p_transfer_rx = NULL,
    .rxi_ipl = 12,
    .txi_ipl = 12,
    .tei_ipl = 12,
    .eri_ipl = 12,
    .rxi_irq = VECTOR_NUMBER_SCI9_RXI,
    .txi_irq = VECTOR_NUMBER_SCI9_TXI,
    .tei_irq = VECTOR_NUMBER_SCI9_TEI,
    .eri_irq = VECTOR_NUMBER_SCI9_ERI,
};

// ── RX queue + drain action ──────────────────────────────────────
static BQUEUE(32, limb_rxq);

static void limb_uart_rx_action(void) {
    while (qbq(limb_rxq) > 0) {
        limb_bus_feed_rx(pullbq(limb_rxq));
    }
}

static uint32_t g_limb_evt_rx, g_limb_evt_txc, g_limb_evt_other;

static void limb_uart_callback(uart_callback_args_t *p_args) {
    if (p_args->event == UART_EVENT_RX_CHAR) {
        g_limb_evt_rx++;
        bool was_empty = (qbq(limb_rxq) == 0);
        pushbq((uint8_t)p_args->data, limb_rxq);
        if (was_empty) {
            later(limb_uart_rx_action);
        }
    } else if (p_args->event == UART_EVENT_TX_COMPLETE) {
        g_limb_evt_txc++;
    } else {
        g_limb_evt_other++;
    }
}

static void limb_uart_send(const uint8_t *buf, size_t len) {
    R_SCI_UART_Write((uart_ctrl_t *)&g_limb_uart_ctrl, (uint8_t const *)buf, (uint32_t)len);
}

static const limb_bus_hw_t hw = { .send = limb_uart_send };

void limb_uart_init(void) {
    R_IOPORT_PinCfg(&g_ioport_ctrl, LIMB_TX_PIN,
        (uint32_t)IOPORT_CFG_PERIPHERAL_PIN | (uint32_t)IOPORT_PERIPHERAL_SCI1_3_5_7_9);
    R_IOPORT_PinCfg(&g_ioport_ctrl, LIMB_RX_PIN,
        (uint32_t)IOPORT_CFG_PERIPHERAL_PIN | (uint32_t)IOPORT_PERIPHERAL_SCI1_3_5_7_9);

    g_limb_baud_err = R_SCI_UART_BaudCalculate(LIMB_UART_BAUD, false, 5000U, &g_limb_baud_setting);
    g_limb_open_err = R_SCI_UART_Open((uart_ctrl_t *)&g_limb_uart_ctrl, (uart_cfg_t *)&g_limb_cfg);

    limb_bus_bind_hw(&hw);
    limb_bus_init();
}

void limb_uart_cli_status(void) {
    print("limb_uart: baud err=");
    printDec0((unsigned)g_limb_baud_err);
    print(" open err=");
    printDec0((unsigned)g_limb_open_err);
    print(" evt rx=");
    printDec0(g_limb_evt_rx);
    print(" txc=");
    printDec0(g_limb_evt_txc);
    print(" other=");
    printDec0(g_limb_evt_other);
    printCr();
}

#else // !LIMB_UART_DEV

#include "cli.h"
#include "printers.h"

void limb_uart_init(void) {
    // Inert in every other environment -- limb_bus stays unbound, so its
    // CLI word (limb-ping) resolves but has no hardware to talk to.
}

void limb_uart_cli_status(void) {
    print("limb_uart: not built in this firmware (needs env:nano_r4_limb)\n");
}

#endif // LIMB_UART_DEV
