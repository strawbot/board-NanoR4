// Board/dac_adc_test.c — `dactest`: DAC0 -> ADC loopback sanity check.
//
// DAC0's output pin P014 is also ADC channel AN009 (Nano A0), so the ADC can
// read the DAC with no wiring. Both are ratiometric to AVCC0 (DAC out =
// code/4096 * AVCC0; ADC raw = V/AVCC0 * 16383), so the expected raw code is
// code * 16383/4095 independent of the real AVCC0. That checks the ADC and
// DAC against each other (offset, gain, linearity), not the absolute
// reference; put a meter on A0 for that: expected mV = code/4096 * AVCC0.
//
// Datasheet (RA4M1 Table 2.52): DAC output range 0.35 V .. AVCC0 - 0.47 V,
// offset and full-scale error +-30 mV, INL +-8 LSB, load >= 30 kOhm. Codes
// below stay inside that range; the tolerance covers the +-30 mV offset.
//
// Runs as a cooperative state machine (in()); nothing blocks. A0 must not be
// connected to a sense resistor while it runs: the DAC would drive it.

#include "tea.h"
#include "cli.h"
#include "printers.h"
#include "board_cli.h"

#define SETTLE_MS  5      // DAC settles in 30 us; this covers several ADC scans
#define TOL_MV     40U    // +-30 mV DAC offset plus ADC noise

static const uint16_t codes[] = { 512, 1024, 2048, 3072, 3584 };
#define NCODES (sizeof codes / sizeof codes[0])

static uint8_t idx;
static bool    running, named;
static uint8_t fails;
static bool    all_low;   // true while every step so far read under 100 counts

static void dactest_step(void);

static void dactest_start_step(void) {
    dac_write_code(codes[idx]);
    in(msec(SETTLE_MS), dactest_step);
}

static void dactest_step(void) {
    uint32_t code = codes[idx];
    uint32_t vref = adc_vref_get();
    uint32_t exp_raw = code * 16383U / 4095U;
    uint32_t raw     = adc_read_an_raw(9);
    int32_t  err     = (int32_t)raw - (int32_t)exp_raw;
    int32_t  err_mv  = err * (int32_t)vref / 16383;
    bool ok = (err_mv >= 0 ? (uint32_t)err_mv : (uint32_t)-err_mv) <= TOL_MV;
    if (!ok) fails++;
    if (raw >= 100U) all_low = false;

    print("  "); printDec(code);
    print("  dac pin ~"); printDec0(code * vref / 4096U); print(" mV");
    print("  raw ");      printDec0(raw);
    print("  expect ");   printDec0(exp_raw);
    print("  err ");      printDec0((unsigned)(err < 0 ? -err : err));
    print(err < 0 ? " low" : " high");
    print(ok ? "  ok" : "  OUT OF TOLERANCE");
    printCr();

    // A0 not following the very first step means something is loading the
    // DAC: stop now rather than drive a heavy load through four more steps.
    if (idx == 0 && raw < 100U) { idx = NCODES; }
    else if (++idx < NCODES) { dactest_start_step(); return; }
    dac_release();
    running = false;
    if (all_low) {
        print("dactest: A0 never left ~0 V - the DAC is probably loaded. Is a sense");
        printCr();
        print("         resistor (or anything under 30 kOhm) still wired to A0?"); printCr();
    }
    print(fails ? "dactest: MISMATCH - ADC and DAC disagree; see the rows above"
                : "dactest: ok - ADC and DAC agree (ratiometric)");
    printCr();
    print("dactest: DAC0 output stopped, A0 is an analog input again"); printCr();
}

void dactest_cli(void) {
    if (running) { print("dactest: already running"); printCr(); return; }
    if (!named) { named = true; namedAction(dactest_step); }
    print("dactest: DAC0 -> A0 loopback (disconnect any sense resistor on A0)"); printCr();
    print("  code  expected/actual (AVCC0 = "); printDec0(adc_vref_get()); print(" mV)"); printCr();
    idx = 0; fails = 0; all_low = true; running = true;
    dactest_start_step();
}
