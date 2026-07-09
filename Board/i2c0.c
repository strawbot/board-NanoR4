// Board/i2c0.c — IIC0 master driver (SCL=P400, SDA=P401), 400 kHz Fast-mode.
//
// AD5593R (ADC/DAC/GPIO expander) lives on this bus — see
// Robot/io/ad5593r/ad5593r.c for the chip-level protocol; this file only
// owns the RA4M1 IIC0 peripheral.
//
// FSP configuration required (RASC / e2 studio)
// ----------------------------------------------
// This file defines the IIC master instance manually (no RASC regen
// needed to build), but a regenerated project must reproduce:
//   - P400 -> Peripheral, PSEL = IIC  (SCL0 — fixed by silicon on this
//     package, see PinCfgR7FA4M1AxxxFM: iic0.scl.p400)
//   - P401 -> Peripheral, PSEL = IIC  (SDA0 — iic0.sda.p401)
//   - An IIC Master (r_iic_master) stack on channel 0, with RXI/TXI/TEI/ERI
//     interrupts enabled (ra_gen/vector_data.{h,c} carries the four IIC0
//     vector table entries already).
//
// Clock settings
// ---------------
// PCLKB = 32 MHz (see ra_gen/bsp_clock_cfg.h: PLL 64 MHz / ICLK,PCLKB div 2).
// cks=1, brh=15, brl=16 are the values RASC's bit-rate calculator produces
// for a 400 kHz Fast-mode target at PCLKB=32 MHz (rise/fall 120 ns, 50%
// duty): actual rate ~392 kHz, ~49% duty — within I2C Fast-mode spec.
//
// R_IIC_MASTER is interrupt driven; the callback below just flags
// completion so the blocking wrappers can spin until done. AD5593R
// transactions are a handful of bytes at 400 kHz (tens of microseconds),
// so a bounded spin is a reasonable trade against building out a full
// non-blocking state machine for what is, today, a boot-time/CLI-only
// peripheral.

#include "r_iic_master.h"
#include "hal_data.h"
#include "vector_data.h"
#include "i2c0.h"
#include <stddef.h>

static iic_master_instance_ctrl_t g_i2c0_ctrl;   // DO NOT pre-initialize (FSP requirement)

static const iic_master_extended_cfg_t g_i2c0_cfg_extend = {
    .timeout_mode    = IIC_MASTER_TIMEOUT_MODE_SHORT,
    .timeout_scl_low = IIC_MASTER_TIMEOUT_SCL_LOW_ENABLED,
    .clock_settings  = {
        .cks_value = 1,
        .brh_value = 15,
        .brl_value = 16,
    },
};

static volatile bool g_i2c0_done;
static volatile bool g_i2c0_error;

static void i2c0_callback(i2c_master_callback_args_t *p_args) {
    g_i2c0_error = (p_args->event == I2C_MASTER_EVENT_ABORTED);
    g_i2c0_done  = true;
}

static const i2c_master_cfg_t g_i2c0_cfg = {
    .channel       = 0,
    .rate          = I2C_MASTER_RATE_FAST,
    .slave         = 0,
    .addr_mode     = I2C_MASTER_ADDR_MODE_7BIT,
    .ipl           = 12,   // matches ipl used by every other enabled interrupt on this board
    .rxi_irq       = VECTOR_NUMBER_IIC0_RXI,
    .txi_irq       = VECTOR_NUMBER_IIC0_TXI,
    .tei_irq       = VECTOR_NUMBER_IIC0_TEI,
    .eri_irq       = VECTOR_NUMBER_IIC0_ERI,
    .p_transfer_tx = NULL,
    .p_transfer_rx = NULL,
    .p_callback    = i2c0_callback,
    .p_context     = NULL,
    .p_extend      = &g_i2c0_cfg_extend,
};

void i2c0_init(void) {
    R_IIC_MASTER_Open(&g_i2c0_ctrl, &g_i2c0_cfg);
}

// Bounded spin — a stuck bus (no ACK, clock stretched forever) must not
// hang the scheduler. 1,000,000 iterations is generous slack over the
// tens-of-microseconds a real transfer takes at 400 kHz.
static bool i2c0_wait(void) {
    uint32_t spins = 0;
    while (!g_i2c0_done) {
        if (++spins > 1000000UL) {
            R_IIC_MASTER_Abort(&g_i2c0_ctrl);
            return false;
        }
    }
    return !g_i2c0_error;
}

bool i2c0_write(uint8_t addr7, const uint8_t *data, uint32_t len) {
    g_i2c0_done  = false;
    g_i2c0_error = false;
    R_IIC_MASTER_SlaveAddressSet(&g_i2c0_ctrl, addr7, I2C_MASTER_ADDR_MODE_7BIT);
    if (FSP_SUCCESS != R_IIC_MASTER_Write(&g_i2c0_ctrl, (uint8_t *)data, len, false)) {
        return false;
    }
    return i2c0_wait();
}

bool i2c0_read(uint8_t addr7, uint8_t *data, uint32_t len) {
    g_i2c0_done  = false;
    g_i2c0_error = false;
    R_IIC_MASTER_SlaveAddressSet(&g_i2c0_ctrl, addr7, I2C_MASTER_ADDR_MODE_7BIT);
    if (FSP_SUCCESS != R_IIC_MASTER_Read(&g_i2c0_ctrl, data, len, false)) {
        return false;
    }
    return i2c0_wait();
}
