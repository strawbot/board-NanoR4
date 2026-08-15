// Board/limb_io.c — see limb_io.h for the API contract and rationale.

#include "hal_data.h"
#include "limb_io.h"
#include "i2c1.h"
#include "ads1015.h"
#include "mcp4728.h"
#include "mpu6050.h"
#include "mcp4728_addr_assign.h"
#include "printers.h"   // progress output for the isolated-read/sequence/verify diagnostics — same exception as Limbs/muscle_wire.c's cal_step
#include <stddef.h>

// Full 16-channel + IMU sweep cadence. Retune once a planning layer cares
// about latency; 16 ADS1015 conversions at ~2ms each plus I2C overhead
// fits comfortably inside 100ms.
#define SWEEP_PERIOD_MS 100U

// A limb with no ADS1015 wired up NACKs every one of its 4 channels, each
// triggering i2c1's bus-recovery-and-retry (bit-banged GPIO reconfig +
// R_IIC_MASTER_Close/Open) — real, avoidable cost during bring-up with
// only some limbs populated. Back off polling a limb once it's failed
// consistently, instead of retrying it every single sweep.
#define ADC_FAIL_BACKOFF_THRESHOLD 3U
#define ADC_FAIL_BACKOFF_SWEEPS    20U   // ~2s at 100ms/sweep before re-probing

#define MCP4728_DEFAULT_ADDR7 0x60U   // shared factory-default address all 4 chips start at

// limb i = ADS1015 at g_adc_addr[i] + MCP4728 targeting g_dac_addr[i] via
// SEL pin g_sel_pin[i] — see the project's I2C limb-array plan for the
// board wiring this reflects.
static const uint8_t g_adc_addr[LIMB_IO_LIMBS] = { 0x48U, 0x49U, 0x4AU, 0x4BU };
static const uint8_t g_dac_addr[LIMB_IO_LIMBS] = { MCP4728_DEFAULT_ADDR7, 0x61U, 0x62U, 0x63U };
static const bsp_io_port_pin_t g_sel_pin[LIMB_IO_LIMBS] = {
    BSP_IO_PORT_01_PIN_05,   // SEL1, P105 -> limb0 (0x60)
    BSP_IO_PORT_01_PIN_04,   // SEL2, P104 -> limb1 (0x61)
    BSP_IO_PORT_01_PIN_03,   // SEL3, P103 -> limb2 (0x62)
    BSP_IO_PORT_01_PIN_02,   // SEL4, P102 -> limb3 (0x63)
};

static const ads1015_hw_t g_ads1015_hw = { .i2c_write = i2c1_write, .i2c_read = i2c1_read };
static const mcp4728_hw_t g_mcp4728_hw = { .i2c_write = i2c1_write, .i2c_read = i2c1_read };
static const mpu6050_hw_t g_mpu6050_hw = { .i2c_write = i2c1_write, .i2c_read = i2c1_read };

static uint16_t g_adc_code[LIMB_IO_CHANNELS];
static uint16_t g_dac_code[LIMB_IO_CHANNELS];
static mpu6050_data_t g_imu;
static bool g_imu_valid;
static uint8_t g_adc_fail[LIMB_IO_LIMBS];
static uint8_t g_dac_fail[LIMB_IO_LIMBS];
static uint8_t g_imu_fail;
static uint8_t g_adc_skip[LIMB_IO_LIMBS];   // sweeps left to skip this limb entirely, see ADC_FAIL_BACKOFF_* above

// Per-limb step sequence: channel 0 gets converted twice, once first
// (right after the address switch from the IMU/previous limb — this one
// is the one that's been reading stale, see sweep_poll's history below)
// and once again last (right after this limb's own channel 3 — same
// address, no switch, same as channels 1-3 which have always been fine).
// Both results get stored to the same g_adc_code slot, so the second
// (reliable) one simply overwrites the first (unreliable) one — no
// separate discard-vs-keep bookkeeping needed.
#define SWEEP_STEPS_PER_LIMB 5U
static const uint8_t g_step_channel[SWEEP_STEPS_PER_LIMB] = { 0U, 1U, 2U, 3U, 0U };

static uint8_t g_sweep_limb;
static uint8_t g_sweep_step;   // 0-4, see g_step_channel

Event limb_io_ready = { no_action };

// ── non-blocking sweep state machine ────────────────────────────────────
static void sweep_tick(void);

static void advance_and_continue(void) {
    g_sweep_step++;
    if (g_sweep_step >= SWEEP_STEPS_PER_LIMB) {
        g_sweep_step = 0U;
        g_sweep_limb++;
    }
    if (g_sweep_limb >= LIMB_IO_LIMBS) {
        g_sweep_limb = 0U;
        if (mpu6050_read_all(MPU6050_ADDR, &g_imu)) {
            g_imu_valid = true;
            g_imu_fail  = 0U;
        } else if (g_imu_fail < 255U) {
            g_imu_fail++;
        }
        now(*limb_io_ready);
        in(msec(SWEEP_PERIOD_MS), sweep_tick);
        return;
    }
    later(sweep_tick);
}

static void adc_note_fail(uint8_t limb) {
    if (g_adc_fail[limb] < 255U) {
        g_adc_fail[limb]++;
    }
    if (g_adc_fail[limb] >= ADC_FAIL_BACKOFF_THRESHOLD) {
        g_adc_skip[limb] = ADC_FAIL_BACKOFF_SWEEPS;
    }
}

static void sweep_read(void) {
    uint16_t code;
    uint8_t ch = g_step_channel[g_sweep_step];
    if (ads1015_read_conversion(g_adc_addr[g_sweep_limb], &code)) {
        g_adc_code[g_sweep_limb * 4U + ch] = code;
        g_adc_fail[g_sweep_limb] = 0U;
    } else {
        adc_note_fail(g_sweep_limb);
    }
    advance_and_continue();
}

// Poll the chip's own OS "conversion done" bit (Config register bit 15,
// same register limb_io_debug_ch0/i2cregs reads) instead of guessing a
// fixed wait duration, bounded so a non-responding chip can't hang the
// sweep. This alone did NOT fix the channel-0 staleness (bench-confirmed:
// polling still returned stale data) — see g_step_channel above for what
// actually did: reordering so channel 0's authoritative read never
// follows an address switch in the first place.
#define ADS1015_POLL_INTERVAL_MS 1U
#define ADS1015_POLL_MAX_TRIES   10U   // 10ms worst case — generous over the ~0.7ms nominal conversion time

static uint8_t g_sweep_poll_tries;

static void sweep_poll(void) {
    uint16_t cfg;
    bool ok = ads1015_read_config(g_adc_addr[g_sweep_limb], &cfg);
    if (ok && (cfg & 0x8000U)) {   // OS=1: conversion done, safe to read the result
        sweep_read();
        return;
    }
    g_sweep_poll_tries++;
    if (!ok || g_sweep_poll_tries >= ADS1015_POLL_MAX_TRIES) {
        // Gave up waiting for OS=1 — read whatever's there now rather
        // than hang the sweep. adc_fail isn't bumped here: the I2C
        // transactions themselves are succeeding (ok=true in the common
        // case), this is just "conversion took longer than we allotted".
        sweep_read();
        return;
    }
    after(msec(ADS1015_POLL_INTERVAL_MS), sweep_poll);
}

static void sweep_tick(void) {
    if (g_sweep_step == 0U && g_adc_skip[g_sweep_limb] > 0U) {
        g_adc_skip[g_sweep_limb]--;
        g_sweep_step = SWEEP_STEPS_PER_LIMB - 1U;   // skip straight past this limb's steps
        advance_and_continue();
        return;
    }
    if (ads1015_start_conversion(g_adc_addr[g_sweep_limb], g_step_channel[g_sweep_step])) {
        g_sweep_poll_tries = 0U;
        after(msec(ADS1015_POLL_INTERVAL_MS), sweep_poll);
    } else {
        adc_note_fail(g_sweep_limb);
        advance_and_continue();
    }
}

// ── isolated-read diagnostic ─────────────────────────────────────────────
// See limb_io.h. Bypasses the whole sweep engine — talks to one limb's
// chosen channel repeatedly with nothing else on the bus in between. This
// is what confirmed the boot-warmup fix in limb_io_init(): a single
// isolated read succeeded on the very first try (no address-switch
// adjacency, no discard/retry needed), which is what pointed at "needs
// one real conversion, once, ever" rather than a per-sweep timing issue.

static uint8_t g_isolated_limb;
static uint8_t g_isolated_channel;
static uint8_t g_isolated_remaining;
static uint8_t g_isolated_count;

static void isolated_step(void);

static void isolated_read(void) {
    uint16_t code;
    bool ok = ads1015_read_conversion(g_adc_addr[g_isolated_limb], &code);
    print("  iter "); printDec((Cell)(g_isolated_count - g_isolated_remaining));
    print(": ");
    if (ok) { print("code="); printDec(code); } else { print("READ FAILED"); }
    printCr();

    if (--g_isolated_remaining == 0U) {
        print("isolated read done"); printCr();
        limb_io_resume_sweep();
        return;
    }
    isolated_step();
}

static void isolated_step(void) {
    if (!ads1015_start_conversion(g_adc_addr[g_isolated_limb], g_isolated_channel)) {
        print("  start_conversion FAILED"); printCr();
        if (--g_isolated_remaining == 0U) {
            limb_io_resume_sweep();
            return;
        }
        later(isolated_step);
        return;
    }
    after(msec(ADS1015_CONV_TIME_MS), isolated_read);
}

void limb_io_isolated_read(uint8_t limb, uint8_t channel, uint8_t count) {
    if (limb >= LIMB_IO_LIMBS || channel >= 4U) {
        return;
    }
    if (count < 1U)  count = 1U;
    if (count > 50U) count = 50U;

    limb_io_pause_sweep();
    g_isolated_limb     = limb;
    g_isolated_channel  = channel;
    g_isolated_count    = count;
    g_isolated_remaining = count;
    later(isolated_step);
}

// ── sequence-position diagnostic ─────────────────────────────────────────
// See limb_io.h. Reads all 4 channels of one limb, IMU-primed then
// starting from an arbitrary channel instead of always 0 — run with
// start_channel 0/1/2/3 across separate calls to see whether the
// unreliable one is always position 0 in the printed list (confirms
// "whichever channel is address-switch-adjacent", matching the fix
// applied in the main sweep) or always channel 0 by number (would
// contradict it and point somewhere else entirely).

static uint8_t g_seq_limb;
static uint8_t g_seq_start_ch;
static uint8_t g_seq_pos;   // 0-3, position in this run's printed order

static void seq_step(void);

static void seq_read(void) {
    uint16_t code;
    uint8_t ch = (uint8_t)((g_seq_start_ch + g_seq_pos) % 4U);
    bool ok = ads1015_read_conversion(g_adc_addr[g_seq_limb], &code);
    print("  pos "); printDec(g_seq_pos); print(" (ch"); printDec(ch); print("): ");
    if (ok) { print("code="); printDec(code); } else { print("READ FAILED"); }
    printCr();

    g_seq_pos++;
    if (g_seq_pos >= 4U) {
        print("sequence test done"); printCr();
        limb_io_resume_sweep();
        return;
    }
    seq_step();
}

static void seq_step(void) {
    uint8_t ch = (uint8_t)((g_seq_start_ch + g_seq_pos) % 4U);
    if (!ads1015_start_conversion(g_adc_addr[g_seq_limb], ch)) {
        print("  pos "); printDec(g_seq_pos); print(": start_conversion FAILED"); printCr();
        g_seq_pos++;
        if (g_seq_pos >= 4U) {
            limb_io_resume_sweep();
            return;
        }
        later(seq_step);
        return;
    }
    after(msec(ADS1015_CONV_TIME_MS), seq_read);
}

void limb_io_test_sequence(uint8_t limb, uint8_t start_channel) {
    if (limb >= LIMB_IO_LIMBS) {
        return;
    }
    limb_io_pause_sweep();
    g_seq_limb     = limb;
    g_seq_start_ch = start_channel % 4U;
    g_seq_pos      = 0U;

    // Prime with an IMU read so position 0 reliably reproduces the same
    // "just switched I2C target" scenario the real sweep always hits,
    // regardless of what limb_io_pause_sweep() happened to interrupt.
    mpu6050_data_t discard;
    (void)mpu6050_read_all(MPU6050_ADDR, &discard);

    print("sequence test: limb "); printDec(limb);
    print(" starting at ch"); printDec(g_seq_start_ch); printCr();
    seq_step();
}

// ── verify self-test ─────────────────────────────────────────────────────
// See limb_io.h. Compares the sweep's cached value for each of limb's 4
// channels (what limbio shows right now) against a freshly-taken isolated
// reading, flagging anything beyond ordinary ADC noise. Passing this
// doesn't prove there's no bug anywhere, only that the sweep's cache
// currently agrees with reality — but a MISMATCH is unambiguous evidence
// of a real problem, wherever it's coming from.
#define VERIFY_TOLERANCE_COUNTS 10U   // ~20mV — generous over ordinary ADC noise, tight enough to catch real staleness

static uint16_t g_verify_before[4];
static uint8_t  g_verify_limb;
static uint8_t  g_verify_ch;

static void verify_step(void);

static void verify_read(void) {
    uint16_t code;
    bool ok = ads1015_read_conversion(g_adc_addr[g_verify_limb], &code);
    uint16_t before = g_verify_before[g_verify_ch];
    print("  ch"); printDec(g_verify_ch);
    print("  sweep="); printDec(before);
    print("  fresh=");
    if (ok) {
        printDec(code);
        int delta = (int)code - (int)before;
        if (delta < 0) delta = -delta;
        if ((uint32_t)delta > VERIFY_TOLERANCE_COUNTS) {
            print("  <-- MISMATCH");
        }
    } else {
        print("READ FAILED");
    }
    printCr();

    g_verify_ch++;
    if (g_verify_ch >= 4U) {
        print("verify done"); printCr();
        limb_io_resume_sweep();
        return;
    }
    verify_step();
}

static void verify_step(void) {
    if (!ads1015_start_conversion(g_adc_addr[g_verify_limb], g_verify_ch)) {
        print("  ch"); printDec(g_verify_ch); print(": start_conversion FAILED"); printCr();
        g_verify_ch++;
        if (g_verify_ch >= 4U) {
            limb_io_resume_sweep();
            return;
        }
        later(verify_step);
        return;
    }
    after(msec(ADS1015_CONV_TIME_MS), verify_read);
}

void limb_io_verify(uint8_t limb) {
    if (limb >= LIMB_IO_LIMBS) {
        return;
    }
    for (uint8_t ch = 0U; ch < 4U; ch++) {
        g_verify_before[ch] = limb_io_get_adc((uint8_t)(limb * 4U + ch));
    }
    limb_io_pause_sweep();
    g_verify_limb = limb;
    g_verify_ch   = 0U;
    verify_step();
}

// ── automated pass/fail test harness ─────────────────────────────────────
// See limb_io.h. Three scripted phases, each ending in a real assertion
// (not just a printed number to eyeball):
//   1. all 4 DACs -> 0,        expect all 4 ADCs ~0
//   2. DACs -> 1000/2000/3000/4000 (ch0..ch3), expect ADC[ch] ~= ADC[ch0]*(ch+1)
//   3. all 4 DACs -> 0 again,  expect all 4 ADCs ~0
// Phase 2's ratio check (not an absolute-value check) is deliberate: DAC
// code and ADC code aren't the same unit (different chips, different
// references), but if the DAC-to-ADC path is linear with no channel-
// specific staleness, every channel's reading should scale with its own
// DAC setting relative to channel 0's, regardless of the absolute scale
// factor between them. Reads within each phase start from start_channel
// (wrapping) and are IMU-primed first, same as i2cseq — run with
// start_channel 0/1/2/3 across separate calls for the "is it channel 0,
// or whichever channel reads first" coverage question.
#define HARNESS_SETTLE_MS          20U   // DAC output settle time before reading
#define HARNESS_ZERO_TOLERANCE     10U   // counts, ~20mV — phases 1 and 3
#define HARNESS_RATIO_TOLERANCE_PCT 20U  // phase 2, generous over real DAC/ADC non-ideality

static const uint16_t g_harness_dac[3][4] = {
    { 0U,    0U,    0U,    0U },
    { 1000U, 2000U, 3000U, 4000U },
    { 0U,    0U,    0U,    0U },
};
static const char *const g_harness_phase_name[3] = {
    "zero (before)", "ramp 1000/2000/3000/4000", "zero (after)",
};

static uint8_t  g_harness_limb;
static uint8_t  g_harness_start_ch;
static uint8_t  g_harness_phase;    // 0-2, indexes g_harness_dac/g_harness_phase_name
static uint8_t  g_harness_pos;      // 0-3, read position within current phase
static uint16_t g_harness_adc[4];   // indexed by actual channel number, not position
static bool     g_harness_adc_ok[4];
static uint8_t  g_harness_pass_count;

static void harness_write_phase(void);
static void harness_read_step(void);

static void harness_evaluate_phase(void) {
    print("phase "); printDec(g_harness_phase); print(": "); print(g_harness_phase_name[g_harness_phase]); printCr();

    bool phase_pass = true;
    bool is_ramp = (g_harness_phase == 1U);
    uint16_t base = g_harness_adc[0];

    for (uint8_t ch = 0U; ch < 4U; ch++) {
        print("  ch"); printDec(ch); print(" adc=");
        if (!g_harness_adc_ok[ch]) {
            print("READ FAILED");
            phase_pass = false;
            printCr();
            continue;
        }
        printDec(g_harness_adc[ch]);

        bool ok;
        if (is_ramp) {
            uint32_t expected = (uint32_t)base * (ch + 1U);
            int32_t  diff     = (int32_t)g_harness_adc[ch] - (int32_t)expected;
            if (diff < 0) diff = -diff;
            uint32_t tolerance = (expected * HARNESS_RATIO_TOLERANCE_PCT) / 100U;
            if (tolerance < HARNESS_ZERO_TOLERANCE) tolerance = HARNESS_ZERO_TOLERANCE;
            print("  expected~"); printDec((Cell)expected);
            ok = (uint32_t)diff <= tolerance;
        } else {
            ok = g_harness_adc[ch] <= HARNESS_ZERO_TOLERANCE;
            print("  expected~0");
        }
        print(ok ? "  ok" : "  FAIL");
        if (!ok) phase_pass = false;
        printCr();
    }

    print(phase_pass ? "  PHASE PASS" : "  PHASE FAIL"); printCr();
    if (phase_pass) g_harness_pass_count++;

    g_harness_phase++;
    if (g_harness_phase >= 3U) {
        print("=== harness done: "); printDec(g_harness_pass_count); print("/3 phases passed ==="); printCr();
        limb_io_resume_sweep();
        return;
    }
    harness_write_phase();
}

static void harness_read_result(void) {
    uint16_t code;
    uint8_t  ch = (uint8_t)((g_harness_start_ch + g_harness_pos) % 4U);
    bool ok = ads1015_read_conversion(g_adc_addr[g_harness_limb], &code);
    g_harness_adc_ok[ch] = ok;
    g_harness_adc[ch]    = ok ? code : 0U;

    g_harness_pos++;
    if (g_harness_pos >= 4U) {
        harness_evaluate_phase();
        return;
    }
    harness_read_step();
}

static void harness_read_step(void) {
    uint8_t ch = (uint8_t)((g_harness_start_ch + g_harness_pos) % 4U);
    if (!ads1015_start_conversion(g_adc_addr[g_harness_limb], ch)) {
        g_harness_adc_ok[ch] = false;
        g_harness_adc[ch]    = 0U;
        g_harness_pos++;
        if (g_harness_pos >= 4U) {
            harness_evaluate_phase();
            return;
        }
        later(harness_read_step);
        return;
    }
    after(msec(ADS1015_CONV_TIME_MS), harness_read_result);
}

static void harness_prime_and_read(void) {
    mpu6050_data_t discard;
    (void)mpu6050_read_all(MPU6050_ADDR, &discard);   // same IMU-priming as i2cseq, see there for why
    g_harness_pos = 0U;
    harness_read_step();
}

static void harness_write_phase(void) {
    for (uint8_t sub = 0U; sub < 4U; sub++) {
        uint8_t ch = (uint8_t)(g_harness_limb * 4U + sub);
        limb_io_set_dac(ch, g_harness_dac[g_harness_phase][sub]);
    }
    after(msec(HARNESS_SETTLE_MS), harness_prime_and_read);
}

void limb_io_test_harness(uint8_t limb, uint8_t start_channel) {
    if (limb >= LIMB_IO_LIMBS) {
        return;
    }
    limb_io_pause_sweep();
    g_harness_limb       = limb;
    g_harness_start_ch   = start_channel % 4U;
    g_harness_phase       = 0U;
    g_harness_pass_count = 0U;

    print("=== i2charness: limb "); printDec(limb);
    print(" start_ch "); printDec(g_harness_start_ch); print(" ==="); printCr();
    harness_write_phase();
}

// ── register-level diagnostic ────────────────────────────────────────────
// Three attempts at "fix channel 0" have all failed to actually fix it
// (per-sweep discard/reconvert, immediate boot warmup, delayed boot
// warmup) despite using the exact byte sequence i2ctest's isolated read
// uses successfully. Time to stop guessing at the mechanism and look at
// the actual Config register instead of only the downstream conversion
// result — this shows whether our write (OS/MUX/PGA/MODE/DR bits) is
// landing as intended at all, rather than continuing to speculate blind.

static uint8_t g_debug_limb;

static void debug_print_config(const char *label) {
    uint16_t cfg;
    print(label);
    if (ads1015_read_config(g_adc_addr[g_debug_limb], &cfg)) {
        print("cfg=0x"); printHex4(cfg);
    } else {
        print("READ FAILED");
    }
    printCr();
}

static void debug_step3(void) {
    debug_print_config("  after wait:               ");
    uint16_t code;
    print("  conversion register:      ");
    if (ads1015_read_conversion(g_adc_addr[g_debug_limb], &code)) {
        print("code="); printDec(code);
    } else {
        print("READ FAILED");
    }
    printCr();
    print("debug done"); printCr();
    limb_io_resume_sweep();
}

static void debug_step2(void) {
    debug_print_config("  immediately after start:  ");
    after(msec(ADS1015_CONV_TIME_MS), debug_step3);
}

void limb_io_debug_ch0(uint8_t limb) {
    if (limb >= LIMB_IO_LIMBS) {
        return;
    }
    limb_io_pause_sweep();
    g_debug_limb = limb;

    debug_print_config("  before:                   ");
    if (!ads1015_start_conversion(g_adc_addr[limb], 0U)) {
        print("  start_conversion FAILED"); printCr();
        limb_io_resume_sweep();
        return;
    }
    later(debug_step2);
}

// ── public API ───────────────────────────────────────────────────────────

// No boot warmup here anymore. One used to exist — a delayed, staggered
// per-limb isolated read at startup — to work around channel 0 reading 0
// on the very first conversion ever. It's redundant now: the sweep's
// channel-0 step ordering (g_step_channel above) means channel 0's
// authoritative read always follows channel 3 at the same address, from
// the very first sweep cycle onward, so that first-conversion issue no
// longer has a code path to occur on. Confirmed removing it doesn't
// regress anything.
void limb_io_init(void) {
    ads1015_bind_hw(&g_ads1015_hw);
    mcp4728_bind_hw(&g_mcp4728_hw);
    mpu6050_bind_hw(&g_mpu6050_hw);

    // MCP4728 DAC outputs hold their last-written value across a warm
    // reset (an MCU reset alone doesn't touch the DAC chips at all) — a
    // previous session's leftover actuation values would otherwise still
    // be driving the tendons the moment this boots. Zero all 16 channels
    // immediately, before anything else runs. Absent limbs just fail this
    // quickly, same as any other missing-hardware case.
    for (uint8_t ch = 0U; ch < LIMB_IO_CHANNELS; ch++) {
        limb_io_set_dac(ch, 0U);
    }

    mpu6050_wake(MPU6050_ADDR);

    namedAction(sweep_tick);
    g_sweep_limb = 0U;
    g_sweep_step = 0U;
    in(msec(SWEEP_PERIOD_MS), sweep_tick);
}

void limb_io_pause_sweep(void) {
    // Cancel anything already scheduled, not just future ticks — a
    // paused-but-still-in-flight after() would still touch the bus.
    stop(sweep_tick);
    stop(sweep_read);
}

void limb_io_resume_sweep(void) {
    g_sweep_limb = 0U;
    g_sweep_step = 0U;
    later(sweep_tick);
}

bool limb_io_set_dac(uint8_t ch, uint16_t code12) {
    if (ch >= LIMB_IO_CHANNELS) {
        return false;
    }
    uint8_t limb = ch / 4U;
    uint8_t sub  = ch % 4U;
    if (mcp4728_set_channel(g_dac_addr[limb], sub, code12)) {
        g_dac_code[ch] = code12 & 0x0FFFU;
        g_dac_fail[limb] = 0U;
        return true;
    }
    if (g_dac_fail[limb] < 255U) {
        g_dac_fail[limb]++;
    }
    return false;
}

uint16_t limb_io_get_adc(uint8_t ch) {
    return (ch < LIMB_IO_CHANNELS) ? g_adc_code[ch] : 0U;
}

uint16_t limb_io_get_dac(uint8_t ch) {
    return (ch < LIMB_IO_CHANNELS) ? g_dac_code[ch] : 0U;
}

bool limb_io_get_imu(mpu6050_data_t *out) {
    if (out == NULL || !g_imu_valid) {
        return false;
    }
    *out = g_imu;
    return true;
}

uint8_t limb_io_adc_fail_count(uint8_t limb) {
    return (limb < LIMB_IO_LIMBS) ? g_adc_fail[limb] : 0U;
}

uint8_t limb_io_dac_fail_count(uint8_t limb) {
    return (limb < LIMB_IO_LIMBS) ? g_dac_fail[limb] : 0U;
}

uint8_t limb_io_imu_fail_count(void) {
    return g_imu_fail;
}

bool limb_io_assign_addresses(void) {
    bool ok = true;
    for (uint8_t limb = 0U; limb < LIMB_IO_LIMBS; limb++) {
        ok = mcp4728_addr_assign_one(g_sel_pin[limb], MCP4728_DEFAULT_ADDR7, g_dac_addr[limb]) && ok;
    }
    return ok;
}

bool limb_io_assign_one(uint8_t limb) {
    if (limb >= LIMB_IO_LIMBS) {
        return false;
    }
    return mcp4728_addr_assign_one(g_sel_pin[limb], MCP4728_DEFAULT_ADDR7, g_dac_addr[limb]);
}

// limb_io_reset_one — undo assignment: move limb's MCP4728 from its
// assigned address (g_dac_addr[limb]) back to the shared factory default,
// so it can be reassigned later (to this limb or a different one) via
// limb_io_assign_one(). A no-op (returns true trivially) for limb 0,
// whose assigned address already is the default.
bool limb_io_reset_one(uint8_t limb) {
    if (limb >= LIMB_IO_LIMBS) {
        return false;
    }
    return mcp4728_addr_assign_one(g_sel_pin[limb], g_dac_addr[limb], MCP4728_DEFAULT_ADDR7);
}
