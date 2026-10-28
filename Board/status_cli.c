// Board/status_cli.c — see status_cli.h.
//
// Reads back everything the muscle-drive PCB connects to the Nano: the
// 74HC595 control pins and shadow bytes, the frame timer, and the muscle
// scheduler's windows. The VIN/Vdrain/Vsense analog taps have their own,
// more detailed dump — `pstatus` (Board/power_sense.c) — rather than a
// table here; this board's earlier breadboard/hex-board taps (A0-A3) are
// gone along with that hardware.

#include "status_cli.h"
#include "board_cli.h"
#include "sr595.h"
#include "sr595_hw.h"
#include "muscle.h"
#include "muscle_sr.h"
#include "cli.h"
#include "printers.h"

void status_cli(void) {
    print("\n74HC595 control pins"); printCr();
    sr595_hw_print_pins();

    print("frame timer: ");
    if (muscle_sr_fault() == MUSCLE_SR_OK) print("ok");
    else { print("STOPPED, fault "); printDec0((unsigned)muscle_sr_fault());
           print(" (1 no frame irq, 2 no compare irq, 3 edges missed, 4 emergency stop)"); }
    printCr();
    print("outputs (latched, bit0=QA .. bit7=QH)"); printCr();
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
    print("adc init err = "); printDec0((unsigned)adc_init_status());
    print("  (see pstatus for VIN/Vdrain/Vsense)"); printCr();

    print("adc internals"); printCr();
    adc_print_diag();

    muscle_show();
}
