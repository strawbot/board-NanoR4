// Board/power_sense.c — see power_sense.h.

#include "hal_data.h"
#include "power_sense.h"
#include "vin_sense.h"
#include "adc_sync.h"
#include "board_cli.h"
#include "calib.h"
#include "cli.h"
#include "printers.h"
#include "tea.h"   // sysTicks()

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
#define R_SENSE_MOHM       500UL     // .5R

uint32_t vdrain_tap_mv(void) { return adc_read_an_mv(VDRAIN_AN); }

// Pure math, no ADC access — see vin_sense.c's vin_mv_from_tap() for why:
// a caller holding an already-read tap value (pstatus, reading each channel
// exactly once) computes from that instead of triggering a second
// R_ADC_Read() that ADC_CLEAR_AFTER_READ_ON can turn into a stale/zeroed
// race against the next trigger.
uint32_t vdrain_mv_from_tap(uint32_t tap_mv) {
    return tap_mv * (VDRAIN_R_TOP_OHMS + vdrain_r_bot) / vdrain_r_bot;
}

// Retried the same way power_cli_status() is: adc_sync_wait_ready() only
// guarantees group A wasn't mid-reacquire at the moment it returned, not
// that the read a few cycles later still lands inside that same HIGH
// window — see power_cli_status()'s comment for the full reasoning and
// the bench case (identical bad raw code, twice) that motivated it.
uint32_t vdrain_read_mv(void) {
    uint32_t mv;
    uint8_t tries = 3U;
    do {
        adc_sync_wait_ready();
        mv = vdrain_mv_from_tap(vdrain_tap_mv());
    } while (!adc_sync_marker_ready() && --tries);
    return mv;
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
    calib_save(NULL);
}

// CLI word: vdrain-cal-vin ( -- ) — no meter needed: with the gate off (no
// current anywhere in the path), Vdrain and VIN are the same node
// electrically, so VIN's own reading (already accurate — 100k/20k at
// 0.1%, not the 5% pair Vdrain's divider uses) stands in for a metered
// value. Same solve as vdrain-cal, with vin_read_mv() in place of a
// stack argument.
//
// "No current" can't be confirmed from Vdrain itself (that's the thing
// being calibrated), but Vsense has no divider at all — raw, direct,
// nothing for a 5% part to get wrong — so it's a reference zero-current
// check that doesn't depend on Vdrain's own accuracy. Vsense isn't
// exactly 0 at rest: this project's own bench data (pstatus, gate off,
// all session long) sits at ~8-14mV at idle — not noise that comes and
// goes, a repeatable floor (ADC/reference offset, not current) — so the
// threshold has to clear that, not just "a few mV". 20mV clears the
// observed idle floor with margin while staying far below any real
// current-flow reading seen this session (tens to hundreds of mV).
void power_cli_vdrain_cal_vin(void) {   // ( -- )
    // DADR[0] is DAC12's own target register (Board/analog_test.c's dac12!
    // writes it directly, no cached mv anywhere to read back otherwise) —
    // a live readback of what the gate is actually being TOLD to do,
    // independent of Vsense's noisy, offset-y REPORT of what's actually
    // happening. Checking both catches two different failure modes: this
    // one catches "dac12! was left at a nonzero setpoint" even if Vsense's
    // noise floor happens to look quiet at the exact instant of the call;
    // the Vsense check below still matters on its own (confirms the FET
    // is actually responding, not just that the setpoint register says 0).
    uint32_t dac12_code = R_DAC->DADR[0];
    if (dac12_code != 0U) {
        uint32_t dac12_mv = dac12_code * adc_vref_get() / 4095UL;
        print("vdrain-cal-vin: dac12 is set to "); printDec0(dac12_mv);
        print(" mV (code "); printDec0(dac12_code); print(") -- run 0 dac12! first"); printCr();
        return;
    }
    uint32_t vsense = vsense_read_mv();
    if (vsense > 20U) {
        print("vdrain-cal-vin: Vsense shows "); printDec0(vsense);
        print(" mV -- current is flowing, turn the gate off first"); printCr();
        return;
    }
    uint32_t vin = vin_read_mv();
    uint32_t tap = vdrain_tap_mv();
    if (vin == 0U || tap == 0U || vin <= tap) {
        print("vdrain-cal-vin: bad reading (VIN "); printDec0(vin);
        print(" mV, tap "); printDec0(tap); print(" mV) -- not saved"); printCr();
        return;
    }
    uint32_t r = (uint32_t) ((uint64_t) VDRAIN_R_TOP_OHMS * tap / (vin - tap));
    if (r < 100U || r > 100000U) { print("vdrain-cal-vin: result out of range, not saved"); printCr(); return; }
    vdrain_r_bot = r;
    print("vdrain-cal-vin: R_BOT = "); printDec0(vdrain_r_bot);
    print(" ohms (against VIN "); printDec0(vin); print(" mV)"); printCr();
    calib_save(NULL);
}

// See vdrain_read_mv()'s comment just above for why this retries instead
// of a single wait-then-read.
uint32_t vsense_read_mv(void) {
    uint32_t mv;
    uint8_t tries = 3U;
    do {
        adc_sync_wait_ready();
        mv = adc_read_an_mv(VSENSE_AN);
    } while (!adc_sync_marker_ready() && --tries);
    return mv;
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
//
// Takes rsense_mohm as a parameter (rather than hardcoding R_SENSE_MOHM
// directly here) so power_cli_status() can print the same relation against
// a different installed sense resistor without duplicating the guards —
// see its own call below.
static uint32_t rwire_mohm_at(uint32_t vin_mv, uint32_t vdrain_mv, uint32_t vsense_mv, uint32_t rsense_mohm) {
    if (vsense_mv < 5U)      return 0U;   // too little current to divide by meaningfully
    if (vdrain_mv >= vin_mv) return 0U;   // noise/transient, not a real reading
    return (vin_mv - vdrain_mv) * rsense_mohm / vsense_mv;
}

uint32_t rwire_mohm_from(uint32_t vin_mv, uint32_t vdrain_mv, uint32_t vsense_mv) {  // pure math, no ADC access
    return rwire_mohm_at(vin_mv, vdrain_mv, vsense_mv, R_SENSE_MOHM);
}

// Reads VIN/Vdrain/Vsense together as one atomic snapshot, retried as a
// group — the discipline power_cli_status() already used (see its own
// comment below; both now share this). rwire_read_mohm() used to call
// vin_read_mv()/vdrain_read_mv()/vsense_read_mv() independently: each is
// individually gated against group A's marker, but not against EACH
// OTHER, so they can land on different ~64us cycles. That's fine for
// Vdrain and VIN on their own (both are stable, slow-moving signals a
// one-cycle skew between doesn't change meaningfully), but rwire's own
// division by Vsense is exactly where it bites: a Vdrain from one cycle
// paired with a Vsense from a different, lower-current cycle produces a
// huge, physically-meaningless result — bench-confirmed (two independent
// `7 rwire-med` calls both landing on the exact same 34052 mOhm outlier,
// not plausible as ordinary noise; a median is powerless against it
// since it's systematic, not random, and present in enough of each
// batch's 7 samples to win).
typedef struct {
    uint16_t vin_raw, vdrain_raw, vsense_raw;
    uint32_t vin_tap, vdrain_tap, vsense_mv;
    uint32_t vin_mv, vdrain_mv;
} power_sense_group_t;

static power_sense_group_t power_sense_read_group(void) {
    power_sense_group_t g;
    uint32_t vref;
    uint8_t tries = 3U;
    do {
        adc_sync_wait_ready();   // don't start reading group A mid-reacquire
        vref = adc_vref_get();

        // Group A's own hardware scan order (19 then 20, HWM 35.3.2) —
        // reading them backwards let a tight enough read window straddle
        // an in-progress scan (bench-confirmed: Vdrain and Vsense both
        // reading near-zero in the same row, a combination that isn't
        // physically possible for the real circuit).
        g.vsense_raw = adc_read_an_raw(19U);
        g.vdrain_raw = adc_read_an_raw(20U);
        g.vin_raw    = adc_read_an_raw(21U);
        // VIN is group B (GTCCRB-offset trigger, no P112-equivalent marker
        // — only 4 DMAC channels exist on this part and group A's marker
        // already uses 2), so the marker_ready() check below can't cover
        // it. What's available instead: VIN never legitimately reads
        // exactly 0 while the board is powered, and 0 is exactly what
        // ADC_CLEAR_AFTER_READ_ON leaves between a read and the next
        // ~64us group B trigger refilling it.
    } while ((!adc_sync_marker_ready() || g.vin_raw == 0U) && --tries);

    g.vin_tap    = g.vin_raw    * vref / 16383UL;
    g.vdrain_tap = g.vdrain_raw * vref / 16383UL;
    g.vsense_mv  = g.vsense_raw * vref / 16383UL;
    g.vin_mv     = vin_mv_from_tap(g.vin_tap);
    g.vdrain_mv  = vdrain_mv_from_tap(g.vdrain_tap);
    return g;
}

uint32_t rwire_read_mohm(void) {
    power_sense_group_t g = power_sense_read_group();
    return rwire_mohm_from(g.vin_mv, g.vdrain_mv, g.vsense_mv);
}

void power_cli_vdrain(void) { lit((Cell) vdrain_read_mv()); }
void power_cli_vsense(void) { lit((Cell) vsense_read_mv()); }
void power_cli_isense(void) { lit((Cell) isense_read_ma()); }
void power_cli_rwire(void)  { lit((Cell) rwire_read_mohm()); }

// Plain insertion sort — n is small (bounded below), not worth anything
// fancier.
static void sort_u32(uint32_t *a, uint8_t n) {
    for (uint8_t i = 1; i < n; i++) {
        uint32_t key = a[i];
        int8_t j = (int8_t) i - 1;
        while (j >= 0 && a[(uint8_t) j] > key) { a[(uint8_t) (j + 1)] = a[(uint8_t) j]; j--; }
        a[(uint8_t) (j + 1)] = key;
    }
}

// CLI word: rwire-med ( n -- mohm ) — median of n single-shot rwire@
// reads, for exactly the noise rwire@'s own math can't avoid: at low
// current, VIN-Vdrain is a small difference between two ~20V-scale
// numbers each with their own mV-level jitter, so the result is noise-
// dominated almost regardless of how clean the individual channel reads
// are (bench-confirmed: 409-1727 mOhm swing at a 100mV dac12 setpoint,
// all individually "valid" pstatus rows). A median is also a free side
// benefit against the still-unexplained occasional exact-zero pstatus
// row (Board/adc_sync.c's history/this session's open thread) — as long
// as it's a minority of the n samples, the median drops it along with
// everything else noisy, no special-case zero detection needed.
//
// Bench-caught the hard way: calling rwire_read_mohm() back-to-back in a
// tight loop (no delay) runs far faster than the ~64-68us group A cycle,
// so sample 2+ can land in the SAME window sample 1 already read-and-
// cleared via ADC_CLEAR_AFTER_READ_ON. power_sense_read_group()'s marker
// check only proves "this window's data hasn't been consumed yet" — it
// has no way to know an earlier call in this same loop already consumed
// it microseconds ago. Result: one real sample followed by several
// zeros, often enough of them that the MEDIAN itself comes out 0 (seen
// on the bench: three straight `7 rwire-med .` calls returning 0 while
// pstatus, paced by human typing and so naturally landing in different
// cycles, kept showing ~650 mOhm the whole time). Fix is a floor on the
// gap between samples, comfortably over one cycle, so each one is
// guaranteed to land in a cycle none of this batch has touched yet.
#define RWIRE_MEDIAN_MAX_N 31U
#define RWIRE_MEDIAN_SAMPLE_GAP_US 80U

void power_cli_rwire_median(void) {   // ( n -- mohm )
    Cell v = ret();
    if (v < 1 || v > (Cell) RWIRE_MEDIAN_MAX_N) {
        print("rwire-med: n must be 1-"); printDec0(RWIRE_MEDIAN_MAX_N); printCr();
        lit(0);
        return;
    }
    uint32_t samples[RWIRE_MEDIAN_MAX_N];
    uint8_t n = (uint8_t) v;
    for (uint8_t i = 0; i < n; i++) {
        if (i > 0U) {
            uint32_t start = (uint32_t) sysTicks();
            while (((uint32_t) sysTicks() - start) < 48U * RWIRE_MEDIAN_SAMPLE_GAP_US) { /* spin */ }
        }
        samples[i] = rwire_read_mohm();
    }
    sort_u32(samples, n);
    lit((Cell) samples[n / 2U]);
}

// CLI word: rwire-on-med ( n -- mohm ) — like rwire-med, but skips the
// switching duty cycle's OFF phase instead of letting it vote. Bench-
// confirmed (400mV/1000mV dac12, Vsense visibly pulsing on the scope):
// at real drive levels this isn't noise scattered around one true value,
// it's TWO distinct physical states — conducting (hundreds of mA) and
// idle (the ~8-14mV baseline this project has seen all session) — and
// pstatus/rwire-med catch whichever one happens to be live at an
// essentially random sample phase. A plain median across a mix of both
// states lands on whichever state was the majority in that batch, which
// isn't a meaningful wire resistance at all.
//
// Keeps sampling (same inter-sample gap as rwire-med, same reason) until
// it's collected n ON-phase samples specifically. A sample counts only
// if BOTH:
//   - Vsense clears RWIRE_ON_THRESHOLD_MV — the same 20mV floor
//     vdrain-cal-vin uses for its own "is current flowing" check,
//     comfortably above the ~8-14mV idle baseline this project has seen
//     all session. Needed because rwire_mohm_from() alone isn't enough
//     to tell idle noise from real current: two idle-floor rows with
//     Vsense=9mV, Vdrain=19907mV and VIN differing by just 12mV (noise)
//     computed rwire as 1055 and 388 respectively — nonzero, wildly
//     different, and meaningless, because dividing a single-digit-mV
//     Vsense by a few mV of VIN/Vdrain noise is still just noise. This
//     was the bug in the previous version of this function (checking
//     only the computed result, dropped the Vsense floor entirely).
//   - rwire_mohm_from() itself still comes back nonzero — kept
//     alongside the Vsense check, not instead of it, for the other
//     failure mode this project found separately: right at the
//     conduction threshold, a sample can have real (if small) current
//     while its companion Vdrain drop is small enough that ordinary
//     noise still occasionally pushes Vdrain at-or-above VIN, tripping
//     rwire_mohm_at()'s own sanity guard despite clearing the Vsense
//     floor.
//
// Capped at a fixed number of total tries so a wire that's genuinely
// never conducting (bad setpoint, nothing wired up) can't hang the CLI
// forever; reports how many it actually caught if short of n, rather
// than silently returning a partial-confidence result that looks
// identical to a full one.
#define RWIRE_ON_MEDIAN_MAX_ATTEMPTS (RWIRE_MEDIAN_MAX_N * 10U)
// RWIRE_ON_THRESHOLD_MV itself now lives in power_sense.h — shared with
// wire-sweep (analog_test.c), which hit the exact same OFF-phase garbage
// this function was built to filter.

void power_cli_rwire_on_median(void) {   // ( n -- mohm )
    Cell v = ret();
    if (v < 1 || v > (Cell) RWIRE_MEDIAN_MAX_N) {
        print("rwire-on-med: n must be 1-"); printDec0(RWIRE_MEDIAN_MAX_N); printCr();
        lit(0);
        return;
    }
    uint32_t samples[RWIRE_MEDIAN_MAX_N];
    uint8_t n = (uint8_t) v;
    uint8_t got = 0U;
    uint16_t attempts = 0U;
    while (got < n && attempts < RWIRE_ON_MEDIAN_MAX_ATTEMPTS) {
        if (attempts > 0U) {
            uint32_t start = (uint32_t) sysTicks();
            while (((uint32_t) sysTicks() - start) < 48U * RWIRE_MEDIAN_SAMPLE_GAP_US) { /* spin */ }
        }
        attempts++;
        power_sense_group_t g = power_sense_read_group();
        if (g.vsense_mv <= RWIRE_ON_THRESHOLD_MV) continue;
        uint32_t mo = rwire_mohm_from(g.vin_mv, g.vdrain_mv, g.vsense_mv);
        if (mo != 0U) samples[got++] = mo;
    }
    if (got < n) {
        print("rwire-on-med: only caught "); printDec0(got); print(" of "); printDec0(n);
        print(" ON-phase samples in "); printDec0(attempts); print(" tries"); printCr();
    }
    if (got == 0U) { lit(0); return; }
    sort_u32(samples, got);
    lit((Cell) samples[got / 2U]);
}

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
    // See power_sense_read_group()'s own comment for the full reasoning
    // (why a single atomic read of all three channels, retried as a
    // group, instead of reading each independently) — both this and
    // rwire_read_mohm() share it now, so the discipline can't drift
    // between them the way it did before (this function always read
    // atomically; rwire_read_mohm() didn't, until the shared `34052 mOhm`
    // bug this session traced it to).
    power_sense_group_t g = power_sense_read_group();

    uint32_t isense_ma = isense_ma_from_vsense(g.vsense_mv);
    uint32_t rwire_mo  = rwire_mohm_from(g.vin_mv, g.vdrain_mv, g.vsense_mv);
    // Same relation, hardcoded to the 0.5 ohm (2W) sense resistor rather
    // than whatever R_SENSE_MOHM currently says — that's the part actually
    // installed for the sigmoid/knee sweeps that validated this relation in
    // the first place, independent of which resistor this build currently
    // assumes.
    uint32_t rwire_mo_r5 = rwire_mohm_at(g.vin_mv, g.vdrain_mv, g.vsense_mv, 500UL);

    print("power taps (mV at the pin, raw 14-bit code)"); printCr();

    print("  VIN     AN021 P101 = "); printDec0(g.vin_tap);
    print(" mV  raw "); printDec0(g.vin_raw);
    print("  -> VIN "); printDec0(g.vin_mv);
    print(" mV (R_BOT "); printDec0(vin_rbot_ohms()); print(" ohm; vin-cal to calibrate)"); printCr();

    print("  Vdrain  AN020 P102 = "); printDec0(g.vdrain_tap);
    print(" mV  raw "); printDec0(g.vdrain_raw);
    print("  -> drain "); printDec0(g.vdrain_mv);
    print(" mV (R_BOT "); printDec0(vdrain_rbot_ohms()); print(" ohm; vdrain-cal to calibrate)"); printCr();

    print("  Vsense  AN019 P103 = "); printDec0(g.vsense_mv);
    print(" mV  raw "); printDec0(g.vsense_raw); printCr();

    print("  isense@ "); printDec0(isense_ma); print(" mA   rwire@ ");
    printDec0(rwire_mo); print(" mOhm   rwire@0.5R "); printDec0(rwire_mo_r5); print(" mOhm"); printCr();
}
