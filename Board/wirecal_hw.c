// Board/wirecal_hw.c — see wirecal_hw.h.
//
// The wire under test reads through the same Vsense/Vdrain/VIN circuit as
// Board/power_sense.c's bench rig (AN019/AN020/AN021) — the only current-
// sense circuit actually built on this board. The old breadboard ISNS taps
// (AN000/AN001/AN002/AN009, hex-board plan) are gone: there is no per-
// channel sense wiring to pick between, so `ch` is unused — only one wire
// is ever under test at a time (muscle_sr never overlaps windows, and
// wirecal unfits every other limb for the run), and it is always this one
// physical tap.
//
// Plain adc_read_an_mv() reads, not power_sense.c's vsense_read_mv()/
// vin_read_mv(): those retry against adc_sync_marker_ready(), which only
// moves while the GPT1/ELC-triggered bench carrier (dac8-cmp) is running.
// wirecal samples from muscle_sr's own GPT7 edge instead, with no ELC link
// of its own (yet — see the timing-chain review), so that retry would just
// burn its ~200us budget on every single sample, from inside an interrupt.

#include "wirecal.h"
#include "power_sense.h"
#include "vin_sense.h"
#include "board_cli.h"

// power_sense.c's R_SENSE_MOHM (0.5R, the part actually installed) plus the
// driving FET's Rds(on) and wiring — same margin the old breadboard version
// of this file used. Update if either installed part changes; wirecal only
// needs this close, not exact (it finds the wire's own onset/mid/full
// activation points, not an absolute ohm figure).
#define SERIES_MOHM  530UL

static bool sample(uint8_t ch, uint32_t *ma, uint32_t *vin_mv) {
    (void)ch;
    uint32_t vsense_mv = adc_read_an_mv(19U);   // AN019/P103, direct, no divider
    uint32_t vin_tap    = adc_read_an_mv(21U);  // AN021/P101, VIN divider
    if (vsense_mv == 0U || vin_tap == 0U) return false;   // no fresh conversion (register clears on read)
    *ma     = vsense_mv * 1000UL / 500UL;       // power_sense.c's R_SENSE_MOHM
    *vin_mv = vin_mv_from_tap(vin_tap);
    return true;
}

// Every analog tap right now, for `scope`: Vsense, Vdrain, VIN (before its
// divider), and a spare (this board has only 3 taps now, not 4).
static void taps(uint32_t mv[4]) {
    mv[0] = adc_read_an_mv(19U);   // Vsense
    mv[1] = adc_read_an_mv(20U);   // Vdrain
    mv[2] = adc_read_an_mv(21U);   // VIN tap
    mv[3] = 0U;
}

static const wirecal_hw_t hw = { sample, SERIES_MOHM, taps };

void wirecal_hw_init(void) {
    wirecal_bind_hw(&hw);
    wirecal_init();
}
