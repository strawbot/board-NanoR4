// Board/limb_io.c — see limb_io.h for the API contract and rationale.

#include "hal_data.h"
#include "limb_io.h"
#include "i2c1.h"
#include "ads1015.h"
#include "mcp4728.h"
#include "mpu6050.h"
#include "mcp4728_addr_assign.h"
#include "printers.h"   // progress output for limb_io_isolated_read_ch0 — same exception as Limbs/muscle_wire.c's cal_step
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

static uint8_t g_sweep_limb;
static uint8_t g_sweep_ch;

Event limb_io_ready = { no_action };

// ── non-blocking sweep state machine ────────────────────────────────────
static void sweep_tick(void);

static void advance_and_continue(void) {
    g_sweep_ch++;
    if (g_sweep_ch >= 4U) {
        g_sweep_ch = 0U;
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
    if (ads1015_read_conversion(g_adc_addr[g_sweep_limb], &code)) {
        g_adc_code[g_sweep_limb * 4U + g_sweep_ch] = code;
        g_adc_fail[g_sweep_limb] = 0U;
    } else {
        adc_note_fail(g_sweep_limb);
    }
    advance_and_continue();
}

// Bench-confirmed: it's not a channel-0-specific bug, and it's not really
// about address-switching either (an earlier fix here — discard one
// conversion, reconvert for real, every sweep — did NOT reliably fix it).
// What actually fixes it: one throwaway conversion per limb, done ONCE at
// boot, before the periodic sweep starts — see limb_io_init(). Whatever
// state that clears up on the ADS1015 (or the I2C peripheral) apparently
// stays fixed once cleared; repeating the discard every sweep thereafter
// bought nothing. So sweep_tick is back to one conversion per channel,
// same as every other channel.
static void sweep_tick(void) {
    if (g_sweep_ch == 0U && g_adc_skip[g_sweep_limb] > 0U) {
        g_adc_skip[g_sweep_limb]--;
        g_sweep_ch = 3U;              // skip straight past this limb's 4 channels
        advance_and_continue();
        return;
    }
    if (ads1015_start_conversion(g_adc_addr[g_sweep_limb], g_sweep_ch)) {
        after(msec(ADS1015_CONV_TIME_MS), sweep_read);
    } else {
        adc_note_fail(g_sweep_limb);
        advance_and_continue();
    }
}

// ── isolated-read diagnostic ─────────────────────────────────────────────
// See limb_io.h. Bypasses the whole sweep engine — talks to one limb's
// channel 0 repeatedly with nothing else on the bus in between. This is
// what confirmed the boot-warmup fix in limb_io_init(): a single isolated
// read succeeded on the very first try (no address-switch adjacency, no
// discard/retry needed), which is what pointed at "needs one real
// conversion, once, ever" rather than a per-sweep timing issue.

static uint8_t g_isolated_limb;
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
    if (!ads1015_start_conversion(g_adc_addr[g_isolated_limb], 0U)) {
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

void limb_io_isolated_read_ch0(uint8_t limb, uint8_t count) {
    if (limb >= LIMB_IO_LIMBS) {
        return;
    }
    if (count < 1U)  count = 1U;
    if (count > 50U) count = 50U;

    limb_io_pause_sweep();
    g_isolated_limb     = limb;
    g_isolated_count    = count;
    g_isolated_remaining = count;
    later(isolated_step);
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

// One-time boot warmup — bench-confirmed (via the `i2ctest`/`i2cregs` CLI
// words) that a single isolated channel-0 read permanently fixes that
// limb's readings from then on, including all subsequent normal-sweep
// reads. Two earlier attempts at reproducing that fix with a parallel,
// slightly different implementation (immediate discard-conversion, then a
// standalone delayed-conversion state machine) both failed despite using
// the identical ads1015_start_conversion/read_conversion calls — so
// rather than maintain a second implementation that might differ in some
// way not yet identified, this just calls the SAME proven function
// (limb_io_isolated_read_ch0) that i2ctest uses, once per limb, staggered
// so each finishes (it pauses/resumes the sweep internally) before the
// next starts. If this still doesn't fix it automatically, the remaining
// variable is elapsed time since power-on (the CLI words are always
// human-triggered, so always several-plus seconds post-boot) — bump
// BOOT_WARMUP_DELAY_MS up further.
#define BOOT_WARMUP_DELAY_MS  5000U
#define BOOT_WARMUP_STAGGER_MS 400U   // generous slack over one isolated read's real duration (~10ms)

static uint8_t g_boot_warmup_limb;

static void boot_warmup_tick(void) {
    if (g_boot_warmup_limb >= LIMB_IO_LIMBS) {
        return;
    }
    limb_io_isolated_read_ch0(g_boot_warmup_limb, 1U);
    g_boot_warmup_limb++;
    after(msec(BOOT_WARMUP_STAGGER_MS), boot_warmup_tick);
}

void limb_io_init(void) {
    ads1015_bind_hw(&g_ads1015_hw);
    mcp4728_bind_hw(&g_mcp4728_hw);
    mpu6050_bind_hw(&g_mpu6050_hw);
    mpu6050_wake(MPU6050_ADDR);

    namedAction(sweep_tick);
    g_sweep_limb = 0U;
    g_sweep_ch   = 0U;
    in(msec(SWEEP_PERIOD_MS), sweep_tick);   // sweep runs from boot; boot_warmup_tick self-heals each limb as it fires

    g_boot_warmup_limb = 0U;
    after(msec(BOOT_WARMUP_DELAY_MS), boot_warmup_tick);
}

void limb_io_pause_sweep(void) {
    // Cancel anything already scheduled, not just future ticks — a
    // paused-but-still-in-flight after() would still touch the bus.
    stop(sweep_tick);
    stop(sweep_read);
}

void limb_io_resume_sweep(void) {
    g_sweep_limb = 0U;
    g_sweep_ch   = 0U;
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
