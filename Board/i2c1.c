// Board/i2c1.c — IIC1 master driver (SCL=P100, SDA=P101), 400 kHz Fast-mode.
// See i2c1.h for the bus-wide rationale (why 400 kHz, why this bus exists).
//
// This file defines the IIC master instance manually rather than using
// RASC's generated g_i2c_master1 (ra_gen/hal_data.c) — same reasoning as
// the old IIC0 driver: a hand-rolled instance keeps the driver
// self-contained and independent of whatever else RASC's "I2C Master on
// IIC" stack for channel 1 gets configured to over time. RASC's own IIC1
// pin/vector setup (P100/P101 -> Peripheral/IIC, RXI/TXI/TEI/ERI enabled)
// has already been applied to this project, which is what makes
// VECTOR_NUMBER_IIC1_* below resolve.
//
// Clock settings
// ---------------
// PCLKB is 24 MHz on this board (see ra_gen/bsp_clock_cfg.h: HOCO 48MHz
// ICLK, but PCLKB divided by 2 — RA4M1's datasheet caps PCLKB at 32MHz
// even at "full chip speed", unlike ICLK/PCLKA/PCLKC/PCLKD).
// cks=0, brh=23, brl=24 below are RASC's own bit-rate calculator output
// for PCLKB=24MHz / 400kHz Fast-mode (copied from the g_i2c_master1_extend
// RASC generated in ra_gen/hal_data.c: "Actual calculated bitrate: 394997.
// Actual calculated duty cycle: 49%") — not hand-derived.
//
// R_IIC_MASTER is interrupt driven; the callback below just flags
// completion so the blocking wrappers can spin until done — same
// bounded-spin rationale as the old IIC0 driver (a few bytes at 400 kHz
// is tens of microseconds). On timeout/abort, i2c1_bitbang's recovery
// sequence runs and the transaction is retried once before giving up.

#include "r_iic_master.h"
#include "hal_data.h"
#include "vector_data.h"
#include "project_defs.h"   // sysTicks(), US_TO_SYS()
#include "i2c1.h"
#include "i2c1_bitbang.h"
#include <stddef.h>

static iic_master_instance_ctrl_t g_i2c1_ctrl;   // DO NOT pre-initialize (FSP requirement)

static const iic_master_extended_cfg_t g_i2c1_cfg_extend = {
    .timeout_mode    = IIC_MASTER_TIMEOUT_MODE_SHORT,
    .timeout_scl_low = IIC_MASTER_TIMEOUT_SCL_LOW_ENABLED,
    .clock_settings  = {
        .cks_value = 0,
        .brh_value = 23,
        .brl_value = 24,
    },
};

static volatile bool g_i2c1_done;
static volatile bool g_i2c1_error;

static void i2c1_callback(i2c_master_callback_args_t *p_args) {
    g_i2c1_error = (p_args->event == I2C_MASTER_EVENT_ABORTED);
    g_i2c1_done  = true;
}

static const i2c_master_cfg_t g_i2c1_cfg = {
    .channel       = 1,
    .rate          = I2C_MASTER_RATE_FAST,
    .slave         = 0,
    .addr_mode     = I2C_MASTER_ADDR_MODE_7BIT,
    .ipl           = 12,   // matches ipl used by every other enabled interrupt on this board
    .rxi_irq       = VECTOR_NUMBER_IIC1_RXI,
    .txi_irq       = VECTOR_NUMBER_IIC1_TXI,
    .tei_irq       = VECTOR_NUMBER_IIC1_TEI,
    .eri_irq       = VECTOR_NUMBER_IIC1_ERI,
    .p_transfer_tx = NULL,
    .p_transfer_rx = NULL,
    .p_callback    = i2c1_callback,
    .p_context     = NULL,
    .p_extend      = &g_i2c1_cfg_extend,
};

void i2c1_init(void) {
    R_IIC_MASTER_Open(&g_i2c1_ctrl, &g_i2c1_cfg);
}

// Bounded spin — a stuck bus (no ACK, clock stretched forever, or the
// limb-array board simply not plugged in yet — SDA/SCL floating instead
// of idle-high — means the hardware SCL-low timeout never trips either)
// must not hang the scheduler. This runs synchronously inside a single
// TEA action (limb_io's sweep_tick/sweep_read), and every action on this
// board shares one cooperative scheduler with the CLI's UART RX drain —
// so the bound here isn't just "don't hang forever", it's "don't stall
// keystroke processing for longer than a human notices".
//
// Time-based (not a raw iteration count): 2 ms is ~20-40x a real
// Fast-mode transfer's worst case (tens of microseconds) but still small
// enough, even with a full 4-limb x 4-channel sweep all timing out and
// retrying once each, to keep every single blocking stretch imperceptible
// rather than piling up into multi-hundred-millisecond freezes.
#define I2C1_WAIT_TIMEOUT_US 2000U

static bool i2c1_wait(void) {
    Long start = sysTicks();
    Long limit = (Long)US_TO_SYS(I2C1_WAIT_TIMEOUT_US);
    while (!g_i2c1_done) {
        if ((sysTicks() - start) > limit) {
            R_IIC_MASTER_Abort(&g_i2c1_ctrl);
            return false;
        }
    }
    return !g_i2c1_error;
}

// On failure: bit-banged bus recovery, then reopen the FSP driver (Abort
// leaves it unable to start a fresh transaction cleanly) before the one
// retry.
static void i2c1_recover(void) {
    i2c1_bb_recover();
    R_IIC_MASTER_Close(&g_i2c1_ctrl);
    R_IIC_MASTER_Open(&g_i2c1_ctrl, &g_i2c1_cfg);
}

bool i2c1_write(uint8_t addr7, const uint8_t *data, uint32_t len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        g_i2c1_done  = false;
        g_i2c1_error = false;
        R_IIC_MASTER_SlaveAddressSet(&g_i2c1_ctrl, addr7, I2C_MASTER_ADDR_MODE_7BIT);
        if (FSP_SUCCESS == R_IIC_MASTER_Write(&g_i2c1_ctrl, (uint8_t *)data, len, false) && i2c1_wait()) {
            return true;
        }
        i2c1_recover();
    }
    return false;
}

bool i2c1_read(uint8_t addr7, uint8_t *data, uint32_t len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        g_i2c1_done  = false;
        g_i2c1_error = false;
        R_IIC_MASTER_SlaveAddressSet(&g_i2c1_ctrl, addr7, I2C_MASTER_ADDR_MODE_7BIT);
        if (FSP_SUCCESS == R_IIC_MASTER_Read(&g_i2c1_ctrl, data, len, false) && i2c1_wait()) {
            return true;
        }
        i2c1_recover();
    }
    return false;
}

// i2c1_probe — single attempt, no retry, no bus-recovery: a NACK'd address
// is the expected outcome for ~all of the 112 addresses a scan checks, not
// a fault to recover from. i2c1_write/read's retry+recovery pattern exists
// for a real device that's momentarily wedged; running that on every miss
// during a scan would make it needlessly slow for no benefit. One byte,
// discarded — a read (not a write) so probing a device doesn't risk poking
// an unknown register with unknown data.
bool i2c1_probe(uint8_t addr7) {
    uint8_t dummy;
    g_i2c1_done  = false;
    g_i2c1_error = false;
    R_IIC_MASTER_SlaveAddressSet(&g_i2c1_ctrl, addr7, I2C_MASTER_ADDR_MODE_7BIT);
    if (FSP_SUCCESS != R_IIC_MASTER_Read(&g_i2c1_ctrl, &dummy, 1, false)) {
        return false;
    }
    return i2c1_wait();
}
