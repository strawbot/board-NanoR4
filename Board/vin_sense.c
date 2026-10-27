// Board/vin_sense.c — see vin_sense.h.

#include "vin_sense.h"
#include "board_cli.h"
#include "calib.h"
#include "cli.h"
#include "printers.h"

// Was AN002/P002 (A3) — dead since that pin is now AMP0O's own output
// (Board/analog_test.c), not a VIN divider at all; vin@ was silently
// reading the opamp's output through the wrong scaling math. Moved to
// AN021/P101, a genuinely free ADC-capable pin, with a fresh 100k/20k
// divider (both 0.1%) built for this specific purpose rather than
// reusing the old hex-board divider's values.
#define VIN_AN            21U         // P101 = AN021
#define VIN_R_TOP_OHMS    100000UL    // 0.1 % part
#define VIN_R_BOT_NOMINAL 20000UL     // 0.1 % part — tighter than the old 5% R_BOT, but vin-cal still applies

static uint32_t r_bot = VIN_R_BOT_NOMINAL;

static int32_t rbot_get(void) { return (int32_t)r_bot; }

void vin_sense_init(void) {
    calib_register("rbot!", rbot_get);
}

uint32_t vin_tap_mv(void) { return adc_read_an_mv(VIN_AN); }

// Pure math, no ADC access — lets a caller that already has a tap reading
// (e.g. Board/power_sense.c's pstatus, reading each channel exactly once
// per call) compute VIN from it without a second R_ADC_Read(). With
// ADC_CLEAR_AFTER_READ_ON, a second independent read of the same channel
// moments later can land on a since-cleared register before the next
// trigger refills it — confirmed on the bench as self-inconsistent fields
// within one pstatus row (vsense@/isense@ disagreeing, a raw value briefly
// reading near zero) once Vdrain's own divider started converting reliably
// enough to expose it.
uint32_t vin_mv_from_tap(uint32_t tap_mv) {
    return tap_mv * (VIN_R_TOP_OHMS + r_bot) / r_bot;
}

uint32_t vin_read_mv(void) {
    return vin_mv_from_tap(vin_tap_mv());
}

uint32_t vin_rbot_ohms(void) { return r_bot; }

void vin_cli_read(void) { lit((Cell)vin_read_mv()); }

void vin_cli_rbot_set(void) {
    Cell v = ret();
    if (v >= 1000 && v <= 1000000) r_bot = (uint32_t)v;
}

void vin_cli_cal(void) {   // ( mv -- )
    Cell mv  = ret();
    uint32_t tap = vin_tap_mv();
    if (mv <= 0 || (uint32_t)mv <= tap || tap == 0) {
        print("vin-cal: give the metered VIN in mV (must exceed the tap voltage, now ");
        printDec0(tap); print(" mV)"); printCr();
        return;
    }
    // tap = VIN * R_BOT / (R_TOP + R_BOT)  =>  R_BOT = R_TOP * tap / (VIN - tap)
    uint32_t r = (uint32_t)((uint64_t)VIN_R_TOP_OHMS * tap / ((uint32_t)mv - tap));
    if (r < 1000U || r > 1000000U) { print("vin-cal: result out of range, not saved"); printCr(); return; }
    r_bot = r;
    print("vin-cal: R_BOT = "); printDec0(r_bot); print(" ohms"); printCr();
    calib_save();
}
