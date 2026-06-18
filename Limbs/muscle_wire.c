#include "tea.h"
#include "cli.h"
#include "printers.h"
#include <stdint.h>

uint32_t adc_read_mv(void);
void dac_set(void);
Short dac_get(void);

static void dac_put(Short value) { lit(value); dac_set(); }

static Short start_value = 0, last_value = 1;

void calibrate_muscle_wire() {
    static enum {off, start, end, calibrated} state = off;
    switch (state) {
        case off:
        case calibrated:
            state = start;
            print("\nStarting muscle wire calibration...");
            dac_put(0); // Ensure muscle wire is off at the start of calibration
            break;
        case start:
            if (adc_read_mv() > 1000) {
                start_value = dac_get() + 10; // move past curve
                state = end;
                printDec(adc_read_mv());
            }
            break;
        case end:
            if (adc_read_mv() > 3000) {
                last_value = dac_get() - 1; // move back to max tension point
                dac_put(0);
                printDec(last_value);
                print(" calibrated.");
                state = calibrated;
                return;
            }
    }
    if (dac_get() > 500) {
        state = off;
        dac_put(0); // Ensure muscle wire is off if calibration fails
        print("\nCalibration failed: reached max DAC value without sufficient ADC reading.");
        return;
    }
    dac_put(dac_get() + 1);
    after(msec(10), calibrate_muscle_wire);
}

static Short mw_scale(Short p) { return start_value + (last_value - start_value) * p / 100; }

void set_muscle_wire(Short p) { dac_put(p == 0 ? 0 : p > 99 ? last_value : mw_scale(p) ); }

void cli_set_muscle_wire() { set_muscle_wire(ret()); }

void cli_dac_read() { lit(dac_get()); }