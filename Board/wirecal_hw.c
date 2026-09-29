// Board/wirecal_hw.c — see wirecal_hw.h.
//
// Each wire's current is the voltage on its sense resistor's ISNS tap divided
// by the resistor (1 ohm on the hex PCB: mV == mA). Only one wire conducts at
// a time (the muscle scheduler never overlaps windows), so a tap reads that
// wire's current while its window is open.

#include "wirecal.h"
#include "muscle.h"
#include "board_cli.h"
#include "vin_sense.h"

#define SENSE_MOHM   1000UL   // RSENSE, 1 ohm 1 %
#define SERIES_MOHM  1050UL   // RSENSE + AO3400A Rds(on) (~30 mohm) + wiring

// ADC channel of the sense tap for a muscle channel, or -1 for none.
// BREADBOARD: shift register outputs QB, QC (bits 1, 2) share one sense
// resistor read on A0 (AN009); QG, QH (bits 6, 7) share another on A1 (AN000).
// HEX PCB: one tap per 8 channels, so channel / 8 -> {A0 AN009, A1 AN000,
// A2 AN001}; replace this function with that when the PCB is used.
static int tap_an(uint8_t ch) {
    switch (ch) {
    case 1: case 2: return 9;
    case 6: case 7: return 0;
    default:        return -1;
    }
}

static bool sample(uint8_t ch, uint32_t *ma, uint32_t *vin_mv) {
    int an = tap_an(ch);
    if (an < 0) return false;
    uint32_t mv = adc_read_an_mv((uint8_t)an);
    if (mv == 0) return false;               // no fresh conversion (register clears on read)
    *ma     = mv * 1000UL / SENSE_MOHM;
    *vin_mv = vin_read_mv();
    return true;
}

static const wirecal_hw_t hw = { sample, SERIES_MOHM };

void wirecal_hw_init(void) {
    wirecal_bind_hw(&hw);
    wirecal_init();
}
