// Board/sr595_hw.c — Nano R4 pins for the 74HC595 chain (hex-board plan).
//
//   Nano D11  SER    (595 pin 14)
//   Nano D13  SRCLK  (595 pin 11)
//   Nano D9   RCLK   (595 pin 12)  P303, GPT7_B — a timer pin so the PCB
//                                  can later latch from hardware
//
// Plain GPIO for now. D11 = JP2 pin 14 (P109, MOSI), D13 = JP1 pin 1 (P111,
// SCK). ra_gen/pin_data.c sets P109 to the debug (JTAG TDO) function at
// reset; sr595_hw_init() reconfigures it as a GPIO output. SWD only needs
// P108/P300, so J-Link debugging is unaffected.

#include "hal_data.h"
#include "r_ioport.h"
#include "sr595.h"
#include "sr595_hw.h"
#include "printers.h"

#define SR_SER_PIN    BSP_IO_PORT_01_PIN_09   // D11, JP2-14, MOSI
#define SR_SRCLK_PIN  BSP_IO_PORT_01_PIN_11   // D13, JP1-1, SCK
#define SR_RCLK_PIN   BSP_IO_PORT_03_PIN_03   // D9  P303

static void out(bsp_io_port_pin_t pin, bool level) {
    R_IOPORT_PinWrite(&g_ioport_ctrl, pin, level ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}
static void ser_set(bool l)   { out(SR_SER_PIN, l); }
static void srclk_set(bool l) { out(SR_SRCLK_PIN, l); }
static void rclk_set(bool l)  { out(SR_RCLK_PIN, l); }

// Fast paths for the frame-timer interrupt. One 32-bit PCNTR3 write sets and
// resets any pins of a port at once (low half sets, high half resets), so a bit
// of the chain is three writes instead of three driver calls. SER and SRCLK are
// both on port 1, RCLK on port 3. Each write takes a few PCLKB cycles, so a
// 24-bit shift is about 6 microseconds and SRCLK/RCLK pulses are wide enough for
// the 595 (it needs about 20 ns).
#define PIN_BIT(p)  (1UL << ((p) & 0x0FU))      // BSP_IO_PORT_nn_PIN_mm: low nibble is the pin
#define SER_BIT     PIN_BIT(SR_SER_PIN)
#define SRCLK_BIT   PIN_BIT(SR_SRCLK_PIN)
#define RCLK_BIT    PIN_BIT(SR_RCLK_PIN)

static void fast_shift(const uint8_t *bytes, uint8_t chips) {
    for (int c = (int)chips - 1; c >= 0; c--) {
        for (int bit = 7; bit >= 0; bit--) {
            R_PORT1->PCNTR3 = ((bytes[c] >> bit) & 1U) ? SER_BIT : (SER_BIT << 16);   // data
            R_PORT1->PCNTR3 = SRCLK_BIT;                                             // clock high
            R_PORT1->PCNTR3 = SRCLK_BIT << 16;                                       // clock low
        }
    }
}

static void fast_strobe(void) {
    R_PORT3->PCNTR3 = RCLK_BIT;
    R_PORT3->PCNTR3 = RCLK_BIT << 16;
}

static const sr595_hw_t hw = { ser_set, srclk_set, rclk_set, fast_shift, fast_strobe };

bool sr595_hw_init(uint8_t chips) {
    const bsp_io_port_pin_t pins[] = { SR_SER_PIN, SR_SRCLK_PIN, SR_RCLK_PIN };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        R_IOPORT_PinCfg(&g_ioport_ctrl, pins[i],
            (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t)IOPORT_CFG_PORT_OUTPUT_LOW);
    }
    sr595_bind_hw(&hw);
    return sr595_init(chips);
}

// Read back the level on each control pin (output latch as seen at the pad).
void sr595_hw_print_pins(void) {
    static const struct { bsp_io_port_pin_t pin; const char *name; } p[] = {
        { SR_SER_PIN,   "SER   D11 P109" },
        { SR_SRCLK_PIN, "SRCLK D13 P111" },
        { SR_RCLK_PIN,  "RCLK  D9  P303" },
    };
    for (unsigned i = 0; i < sizeof p / sizeof p[0]; i++) {
        bsp_io_level_t lv = BSP_IO_LEVEL_LOW;
        R_IOPORT_PinRead(&g_ioport_ctrl, p[i].pin, &lv);
        print("  "); print(p[i].name); print(" = ");
        printDec0(lv == BSP_IO_LEVEL_HIGH ? 1U : 0U); printCr();
    }
}
