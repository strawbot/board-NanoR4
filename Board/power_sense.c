// Board/power_sense.c — see power_sense.h.

#include "power_sense.h"
#include "vin_sense.h"
#include "board_cli.h"
#include "calib.h"
#include "cli.h"
#include "printers.h"

// Was 82.5k/22k (0.1%): Thevenin ~17.4k ohms at the tap, high enough that
// the ADC's own repeated sampling measurably drooped the node between
// samples — confirmed on the bench (scope showed the dip present at P102
// but absent on the raw, undivided rail). A cap isn't the fix here the way
// it was for VIN (Board/vin_sense.c): Vdrain is a control-loop-relevant
// signal, not just a monitoring readout, and a cap would blur the real
// switching edges this whole trigger/sync effort exists to see. Dropping
// to a 10k/2.2k pair instead cuts the Thevenin impedance to ~1.8k ohms
// (~9.6x lower) with no added RC lag. Only 5% parts on hand at this value
// — same answer as VIN's own R_BOT: calibrate the tolerance out against a
// meter (vdrain-cal) rather than needing precision stock.
#define VDRAIN_AN            20U      // P102 = AN020
#define VDRAIN_R_TOP_OHMS    10000UL  // 5% part
#define VDRAIN_R_BOT_NOMINAL 2200UL   // 5% part — vdrain-cal solves the real value

static uint32_t vdrain_r_bot = VDRAIN_R_BOT_NOMINAL;

static int32_t vdrain_rbot_get(void) { return (int32_t) vdrain_r_bot; }

void power_sense_init(void) {
    calib_register("vdbot!", vdrain_rbot_get);
}

#define VSENSE_AN          19U        // P103 = AN019, direct, no divider

// Sense resistor currently installed on the muscle-wire FET's source, in
// milliohms. 1R (1/4W) was used for every bench characterization so far;
// 0R5 (2W) is the higher-current-headroom option for voltages/currents
// above the 1R part's ~500mA rating (confirmed exceeded during the 20V
// sweeps). UPDATE THIS if the installed part changes — every current and
// resistance figure below silently scales by whatever this is wrong by.
#define R_SENSE_MOHM       1000UL     // 1R

uint32_t vdrain_tap_mv(void) { return adc_read_an_mv(VDRAIN_AN); }

// Pure math, no ADC access — see vin_sense.c's vin_mv_from_tap() for why:
// a caller holding an already-read tap value (pstatus, reading each channel
// exactly once) computes from that instead of triggering a second
// R_ADC_Read() that ADC_CLEAR_AFTER_READ_ON can turn into a stale/zeroed
// race against the next trigger.
uint32_t vdrain_mv_from_tap(uint32_t tap_mv) {
    return tap_mv * (VDRAIN_R_TOP_OHMS + vdrain_r_bot) / vdrain_r_bot;
}

uint32_t vdrain_read_mv(void) {
    return vdrain_mv_from_tap(vdrain_tap_mv());
}

uint32_t vdrain_rbot_ohms(void) { return vdrain_r_bot; }

void power_cli_vdbot_set(void) {   // ( ohms -- ) set R_BOT directly; the calib file runs this at boot
    Cell v = ret();
    if (v >= 100 && v <= 100000) vdrain_r_bot = (uint32_t) v;
}

void power_cli_vdrain_cal(void) {  // ( mv -- ) solve R_BOT from a metered Vdrain, save it
    Cell mv  = ret();
    uint32_t tap = vdrain_tap_mv();
    if (mv <= 0 || (uint32_t) mv <= tap || tap == 0) {
        print("vdrain-cal: give the metered Vdrain in mV (must exceed the tap voltage, now ");
        printDec0(tap); print(" mV)"); printCr();
        return;
    }
    uint32_t r = (uint32_t) ((uint64_t) VDRAIN_R_TOP_OHMS * tap / ((uint32_t) mv - tap));
    if (r < 100U || r > 100000U) { print("vdrain-cal: result out of range, not saved"); printCr(); return; }
    vdrain_r_bot = r;
    print("vdrain-cal: R_BOT = "); printDec0(vdrain_r_bot); print(" ohms"); printCr();
    calib_save();
}

uint32_t vsense_read_mv(void) {
    return adc_read_an_mv(VSENSE_AN);
}

uint32_t isense_ma_from_vsense(uint32_t vsense_mv) {   // pure math, no ADC access
    return vsense_mv * 1000UL / R_SENSE_MOHM;
}

uint32_t isense_read_ma(void) {
    return isense_ma_from_vsense(vsense_read_mv());
}

// R_wire = (VIN - V_drain) / I_sense, same relation validated against the
// clean sigmoid/knee captures earlier in this project's bring-up. Guarded
// against two failure modes rather than returning a division-by-near-zero
// garbage value: no current to divide by (gate off), and a noisy/startup
// sample where V_drain briefly reads above VIN (unsigned subtraction would
// otherwise wrap to a huge value instead of going negative).
uint32_t rwire_mohm_from(uint32_t vin_mv, uint32_t vdrain_mv, uint32_t vsense_mv) {  // pure math, no ADC access
    if (vsense_mv < 5U)      return 0U;   // too little current to divide by meaningfully
    if (vdrain_mv >= vin_mv) return 0U;   // noise/transient, not a real reading
    return (vin_mv - vdrain_mv) * R_SENSE_MOHM / vsense_mv;
}

uint32_t rwire_read_mohm(void) {
    return rwire_mohm_from(vin_read_mv(), vdrain_read_mv(), vsense_read_mv());
}

void power_cli_vdrain(void) { lit((Cell) vdrain_read_mv()); }
void power_cli_vsense(void) { lit((Cell) vsense_read_mv()); }
void power_cli_isense(void) { lit((Cell) isense_read_ma()); }
void power_cli_rwire(void)  { lit((Cell) rwire_read_mohm()); }

// Raw+derived dump of the three new analog taps (replaces the stale hex-board
// A0-A3 table in status_cli.c for this controller's own inputs).
//
// Reads each channel's raw register exactly once, into a local, and derives
// every printed field (tap mV, scaled mV, isense, rwire) from those locals —
// no second R_ADC_Read() of the same channel anywhere in this function.
// Confirmed necessary on the bench, not just tidiness: with
// ADC_CLEAR_AFTER_READ_ON, the old version's extra reads (the raw print,
// plus isense_read_ma()/rwire_read_mohm() each independently re-reading
// vsense/vdrain/vin) could land on a since-cleared register before the next
// ~64us trigger refilled it, producing self-inconsistent fields within one
// row (a raw value near zero next to a derived field computed from a real,
// different sample moments later).
void power_cli_status(void) {
    uint32_t vref = adc_vref_get();

    // Read order matters, not just which channels: within group A, the
    // hardware converts in ascending channel order (19 then 20, HWM
    // 35.3.2), so these are read in that same order rather than VIN/
    // Vdrain/Vsense print order. Reading 20 before 19 (the original order
    // here) let a tight enough read window straddle an in-progress scan —
    // confirmed on the bench: Vdrain and Vsense both reading near-zero in
    // the same row, a combination that isn't physically possible for the
    // real circuit (zero current can't coexist with a near-zero drain
    // voltage — zero current means no drop anywhere, so Vdrain would sit
    // near VIN, not near 0).
    uint16_t vsense_raw = adc_read_an_raw(19U);
    uint16_t vdrain_raw = adc_read_an_raw(20U);
    uint16_t vin_raw    = adc_read_an_raw(21U);

    // Same formula as adc_read_an_mv() (Board/adc.c) — 16383 is its
    // ADC_FULL_SCALE (14-bit). Computed locally from the raw already in
    // hand instead of calling adc_read_an_mv() again.
    uint32_t vin_tap    = vin_raw    * vref / 16383UL;
    uint32_t vdrain_tap = vdrain_raw * vref / 16383UL;
    uint32_t vsense_mv  = vsense_raw * vref / 16383UL;

    uint32_t vin_mv    = vin_mv_from_tap(vin_tap);
    uint32_t vdrain_mv = vdrain_mv_from_tap(vdrain_tap);
    uint32_t isense_ma = isense_ma_from_vsense(vsense_mv);
    uint32_t rwire_mo  = rwire_mohm_from(vin_mv, vdrain_mv, vsense_mv);

    print("power taps (mV at the pin, raw 14-bit code)"); printCr();

    print("  VIN     AN021 P101 = "); printDec0(vin_tap);
    print(" mV  raw "); printDec0(vin_raw);
    print("  -> VIN "); printDec0(vin_mv);
    print(" mV (R_BOT "); printDec0(vin_rbot_ohms()); print(" ohm; vin-cal to calibrate)"); printCr();

    print("  Vdrain  AN020 P102 = "); printDec0(vdrain_tap);
    print(" mV  raw "); printDec0(vdrain_raw);
    print("  -> drain "); printDec0(vdrain_mv);
    print(" mV (R_BOT "); printDec0(vdrain_rbot_ohms()); print(" ohm; vdrain-cal to calibrate)"); printCr();

    print("  Vsense  AN019 P103 = "); printDec0(vsense_mv);
    print(" mV  raw "); printDec0(vsense_raw); printCr();

    print("  isense@ "); printDec0(isense_ma); print(" mA   rwire@ ");
    printDec0(rwire_mo); print(" mOhm"); printCr();
}
