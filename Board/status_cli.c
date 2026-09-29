// Board/status_cli.c — see status_cli.h.
//
// Reads back everything the muscle-drive breadboard/PCB connects to the
// Nano: the 74HC595 control pins and shadow bytes, the sense-resistor and
// supply-divider analog taps, and the muscle scheduler's windows.

#include "status_cli.h"
#include "board_cli.h"
#include "sr595.h"
#include "sr595_hw.h"
#include "muscle.h"
#include "vin_sense.h"
#include "cli.h"
#include "printers.h"

static const struct { uint8_t an; const char *name; } taps[] = {
    { 9, "A0 ISNS_A  AN009 P014" },
    { 0, "A1 ISNS_B  AN000 P000" },
    { 1, "A2 ISNS_C  AN001 P001" },
    { 2, "A3 VIN_S   AN002 P002" },
};

void status_cli(void) {
    print("\n74HC595 control pins"); printCr();
    sr595_hw_print_pins();

    print("outputs (shadow, bit0=QA .. bit7=QH)"); printCr();
    for (uint8_t c = 0; c < SR595_MAX_CHIPS; c++) {
        uint8_t v = sr595_get(c);
        print("  chip "); printDec0(c);
        print(" = 0x");   printHex2(v);
        print(" ");       printBin(v);
        print("  on:");
        for (uint8_t b = 0; b < 8U; b++)
            if (v & (1U << b)) { print(" "); printDec0((uint8_t)(c * 8U + b + 1U)); }
        printCr();
    }

    adc_print_vref();
    print("analog taps (mV at the pin, raw 14-bit code); adc init err = ");
    printDec0((unsigned)adc_init_status()); printCr();
    for (unsigned i = 0; i < sizeof taps / sizeof taps[0]; i++) {
        uint32_t mv = adc_read_an_mv(taps[i].an);
        print("  "); print(taps[i].name); print(" = "); printDec0(mv); print(" mV  raw ");
        printDec0(adc_read_an_raw(taps[i].an));
        if (taps[i].an == 2U) {   // A3: undo the divider to show the supply
            print("  -> VIN "); printDec0(vin_read_mv());
            print(" mV (R_BOT "); printDec0(vin_rbot_ohms()); print(" ohm; vin-cal to calibrate)");
        }
        printCr();
    }

    print("adc internals"); printCr();
    adc_print_diag();

    muscle_show();
}
