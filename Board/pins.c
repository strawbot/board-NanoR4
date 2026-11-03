// Board/pins.c — pin introspection and manual GPIO control for bench debug.
//
// `pins` (the old `pins` word, gpio_dump_all) was a stub: Board/gpio_dump.c
// is a leftover from the STM32 Nucleo/Discovery ports (GPIOA/MODER/AFR
// registers — none of that exists on RA4M1) and is explicitly excluded from
// this board's build in platformio.ini. This file replaces it with a native
// RA4M1 version.
//
// RA4M1's PFS register (R_PFS->PORT[port].PIN[pin].PmnPFS) is the single
// source of truth for a pin's current wiring: PMR (0 = plain GPIO, 1 =
// routed to a peripheral per PSEL), PSEL (which peripheral, when PMR=1),
// PDR (GPIO direction), PODR/PIDR (output/input level), ASEL (analog input
// enabled). Reading it directly — no driver call needed, it's just a
// register — means `pins` shows a pin's *actual* current state regardless
// of which module last touched it, rather than asking each module what it
// thinks it configured. That distinction is exactly what's needed to catch
// a pin claimed by two modules at once (see the P111 entry below).
//
// Pxyy numbering throughout (port x, pin yy, e.g. P101, P111, P303) matches
// this project's own comments and the RA4M1 datasheet's pin names, and
// round-trips cleanly through plain integer division: port = p/100,
// pin = p%100. So a pin can be named at the CLI the exact same way it's
// named in code and in a scope probe's label — `pin@ 111` reads P111.

#include <string.h>
#include "hal_data.h"
#include "r_ioport.h"
#include "pins.h"
#include "cli.h"
#include "printers.h"

// Known pin assignments across this project. `pins` with no filter prints
// only these — an unaliased pin isn't wrong, it just isn't in this table
// yet; add it here when a new pin gets claimed. This is deliberately a
// flat, hand-maintained list (same spirit as Board/gpio_dump.c's old
// alias_table) rather than something generated, so a conflict like the
// P111 one below has somewhere to be written down in plain English.
typedef struct { uint8_t port; uint8_t pin; const char *name; } pin_alias_t;

static const pin_alias_t pin_aliases[] = {
    { 0, 0,  "AMP0+ (AN000) -- analog_test opamp0" },
    { 0, 1,  "AMP0- (AN001) -- analog_test opamp0" },
    { 0, 2,  "AMP0O (AN002) -- analog_test opamp0" },
    { 0, 14, "DAC12 -- analog_test dac12!/dac12-ramp" },
    { 1, 0,  "CMPIN0 (ACMPLP0 IVCMP0) -- analog_test dac8-cmp" },
    { 1, 1,  "VIN tap (AN021) -- vin_sense" },
    { 1, 2,  "Vdrain tap (AN020) -- power_sense" },
    { 1, 3,  "Vsense (AN019, no divider) -- power_sense" },
    { 1, 9,  "SER D11 -- sr595_hw GPIO out, or limb_uart SCI9 TX (env:nano_r4_limb)" },
    { 1, 10, "VCOUT (ACMPLP0 output) -- analog_test dac8-cmp; or limb_uart SCI9 RX (env:nano_r4_limb)" },
    { 1, 11, "SRCLK D13 -- sr595_hw, GPIO out (also the heartbeat LED on board 7DB, clocks.c)" },
    { 1, 12, "scope marker -- adc_sync, DMA-driven GPIO: high=sample valid, low=re-acquiring" },
    { 2, 4,  "LED yellow -- clocks.c blink()" },
    { 3, 3,  "RCLK D9 -- sr595_hw, GPIO out" },
};
#define PIN_ALIASES (sizeof pin_aliases / sizeof pin_aliases[0])

static const char *pin_alias(uint8_t port, uint8_t pin) {
    for (unsigned i = 0; i < PIN_ALIASES; i++)
        if (pin_aliases[i].port == port && pin_aliases[i].pin == pin) return pin_aliases[i].name;
    return "";
}

static void pin_label(char *buf, uint8_t port, uint8_t pin) {
    buf[0] = 'P';
    buf[1] = (char)('0' + port);
    buf[2] = (char)('0' + (pin / 10U));
    buf[3] = (char)('0' + (pin % 10U));
    buf[4] = 0;
}

static void pin_print_row(uint8_t port, uint8_t pin) {
    uint32_t raw  = R_PFS->PORT[port].PIN[pin].PmnPFS;
    uint32_t podr = raw & 1U;
    uint32_t pidr = (raw >> 1) & 1U;
    uint32_t pdr  = (raw >> 2) & 1U;
    uint32_t asel = (raw >> 15) & 1U;
    uint32_t pmr  = (raw >> 16) & 1U;
    uint32_t psel = (raw >> 24) & 0x1FU;

    char label[5];
    pin_label(label, port, pin);
    print(label); tabTo(7);

    if (asel)      print("ANALOG  ");
    else if (pmr)  print("PERIPH  ");
    else           print(pdr ? "GPIO-OUT" : "GPIO-IN ");
    tabTo(17);

    if (pmr && !asel) { print("psel="); printDec0(psel); }
    else              { print("--"); }
    tabTo(26);

    print("in="); printDec0(pidr); print(" out="); printDec0(podr);
    tabTo(40);

    print(pin_alias(port, pin));
    printCr();
}

// CLI word: pins [filter] — dump every aliased pin's live PFS state. An
// optional filter (plain substring, case-sensitive) matches against either
// the Pxyy label or the alias text, e.g. `pins 111` or `pins srclk`.
void pins_cli_show(void) {
    Byte *filter = parseWord(0);
    print("PIN    MODE     PERIPH   LEVEL        ALIAS"); printCr();
    print("--------------------------------------------------------------------------"); printCr();
    for (unsigned i = 0; i < PIN_ALIASES; i++) {
        if (filter && filter[0]) {
            char label[5];
            pin_label(label, pin_aliases[i].port, pin_aliases[i].pin);
            if (!strstr(label, (const char *)filter) && !strstr(pin_aliases[i].name, (const char *)filter)) continue;
        }
        pin_print_row(pin_aliases[i].port, pin_aliases[i].pin);
    }
}

// CLI word: pin@ ( p -- level ) — read a pin's current input level, p =
// port*100 + pin (e.g. 111 for P111). Works regardless of mode: the input
// buffer reads the pad level whether the pin is GPIO, a peripheral output,
// or floating — it's PIDR, the one PFS bit that's always live.
void pins_cli_read(void) {
    Cell p = ret();
    uint8_t port = (uint8_t)(p / 100U), pin = (uint8_t)(p % 100U);
    uint32_t pidr = (R_PFS->PORT[port].PIN[pin].PmnPFS >> 1) & 1U;
    lit((Cell)pidr);
}

// CLI word: pindir! ( dir p -- ) — claim a pin as plain GPIO and set its
// direction: dir 0 = input, 1 = output (output starts low). This is a full
// PmnPFS replace (R_IOPORT_PinCfg -> r_ioport_pfs_write), so it unconditionally
// evicts whatever peripheral currently owns the pin (PMR/PSEL/ASEL all
// cleared) — exactly what you want to grab a pin back for a continuity
// check, and exactly why this must never be called on a pin another module
// is actively relying on (P111/SRCLK, any ADC analog input, etc.) without
// meaning to take it away from that module.
void pins_cli_dir_set(void) {
    Cell p   = ret();
    Cell dir = ret();
    uint8_t port = (uint8_t)(p / 100U), pin = (uint8_t)(p % 100U);
    bsp_io_port_pin_t bsp_pin = (bsp_io_port_pin_t)((port << 8) | pin);
    R_IOPORT_PinCfg(&g_ioport_ctrl, bsp_pin,
        dir ? ((uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t)IOPORT_CFG_PORT_OUTPUT_LOW)
            : (uint32_t)IOPORT_CFG_PORT_DIRECTION_INPUT);
}

// CLI word: pinset! ( level p -- ) — write a GPIO-output pin's level.
// Doesn't change direction/mode; warns (but still writes PODR) if the pin
// isn't currently configured as a GPIO output, since in that case this has
// no electrical effect at the pad — run `pindir! 1 p` first.
void pins_cli_set(void) {
    Cell p     = ret();
    Cell level = ret();
    uint8_t port = (uint8_t)(p / 100U), pin = (uint8_t)(p % 100U);
    uint32_t raw = R_PFS->PORT[port].PIN[pin].PmnPFS;
    bool is_gpio_out = ((raw >> 16) & 1U) == 0U && ((raw >> 2) & 1U) == 1U;

    bsp_io_port_pin_t bsp_pin = (bsp_io_port_pin_t)((port << 8) | pin);
    R_IOPORT_PinWrite(&g_ioport_ctrl, bsp_pin, level ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);

    if (!is_gpio_out) {
        char label[5]; pin_label(label, port, pin);
        print("pinset!: "); print(label);
        print(" isn't a GPIO output right now (pindir! 1 to make it one) -- wrote PODR, pad won't move");
        printCr();
    }
}
