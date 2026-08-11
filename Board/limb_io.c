// Board/limb_io.c — see limb_io.h for the API contract and rationale.

#include "hal_data.h"
#include "limb_io.h"
#include "i2c1.h"
#include "ads1015.h"
#include "mcp4728.h"
#include "mpu6050.h"
#include "mcp4728_addr_assign.h"
#include <stddef.h>

// Full 16-channel + IMU sweep cadence. Retune once a planning layer cares
// about latency; 16 ADS1015 conversions at ~2ms each plus I2C overhead
// fits comfortably inside 100ms.
#define SWEEP_PERIOD_MS 100U

// limb i = ADS1015 at g_adc_addr[i] + MCP4728 targeting g_dac_addr[i] via
// SEL pin g_sel_pin[i] — see the project's I2C limb-array plan for the
// board wiring this reflects.
static const uint8_t g_adc_addr[LIMB_IO_LIMBS] = { 0x48U, 0x49U, 0x4AU, 0x4BU };
static const uint8_t g_dac_addr[LIMB_IO_LIMBS] = { 0x60U, 0x61U, 0x62U, 0x63U };
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

static void sweep_read(void) {
    uint16_t code;
    if (ads1015_read_conversion(g_adc_addr[g_sweep_limb], &code)) {
        g_adc_code[g_sweep_limb * 4U + g_sweep_ch] = code;
        g_adc_fail[g_sweep_limb] = 0U;
    } else if (g_adc_fail[g_sweep_limb] < 255U) {
        g_adc_fail[g_sweep_limb]++;
    }
    advance_and_continue();
}

static void sweep_tick(void) {
    if (ads1015_start_conversion(g_adc_addr[g_sweep_limb], g_sweep_ch)) {
        after(msec(ADS1015_CONV_TIME_MS), sweep_read);
    } else {
        if (g_adc_fail[g_sweep_limb] < 255U) {
            g_adc_fail[g_sweep_limb]++;
        }
        advance_and_continue();
    }
}

// ── public API ───────────────────────────────────────────────────────────

void limb_io_init(void) {
    ads1015_bind_hw(&g_ads1015_hw);
    mcp4728_bind_hw(&g_mcp4728_hw);
    mpu6050_bind_hw(&g_mpu6050_hw);
    mpu6050_wake(MPU6050_ADDR);

    g_sweep_limb = 0U;
    g_sweep_ch   = 0U;
    namedAction(sweep_tick);
    in(msec(SWEEP_PERIOD_MS), sweep_tick);
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
        ok = mcp4728_addr_assign_one(g_sel_pin[limb], g_dac_addr[limb]) && ok;
    }
    return ok;
}

bool limb_io_assign_one(uint8_t limb) {
    if (limb >= LIMB_IO_LIMBS) {
        return false;
    }
    return mcp4728_addr_assign_one(g_sel_pin[limb], g_dac_addr[limb]);
}
