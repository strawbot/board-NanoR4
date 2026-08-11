// Board/i2c1_bitbang.c — see i2c1_bitbang.h.

#include "hal_data.h"
#include "r_ioport.h"
#include "project_defs.h"   // sysTicks(), US_TO_SYS()
#include "i2c1_bitbang.h"

#define I2C1_SCL_PIN BSP_IO_PORT_01_PIN_00   // P100
#define I2C1_SDA_PIN BSP_IO_PORT_01_PIN_01   // P101

// ~50 kHz bit rate -> 10us half-bit-period. Generous margin under the
// 400kHz Fast-mode this command is speced to tolerate (datasheet: "highest
// clock rate of this command is 400 kHz"), so software delay jitter here
// can't violate any I2C timing spec.
#define I2C1_BB_HALF_BIT_US 10U

void i2c1_bb_delay(void) {
    Long start = sysTicks();
    Long target = (Long)US_TO_SYS(I2C1_BB_HALF_BIT_US);
    while ((sysTicks() - start) < target) { /* busy wait */ }
}

static void scl_drive(bool high) {
    R_IOPORT_PinCfg(&g_ioport_ctrl, I2C1_SCL_PIN,
        (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT |
        (high ? (uint32_t)IOPORT_CFG_PORT_OUTPUT_HIGH : (uint32_t)IOPORT_CFG_PORT_OUTPUT_LOW));
}

// Open-drain emulation: "high" releases the line (input, pulled up
// externally); "low" drives it low. Never drives high directly.
static void sda_drive(bool high) {
    if (high) {
        R_IOPORT_PinCfg(&g_ioport_ctrl, I2C1_SDA_PIN, (uint32_t)IOPORT_CFG_PORT_DIRECTION_INPUT);
    } else {
        R_IOPORT_PinCfg(&g_ioport_ctrl, I2C1_SDA_PIN,
            (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t)IOPORT_CFG_PORT_OUTPUT_LOW);
    }
}

static bool sda_read(void) {
    bsp_io_level_t level;
    R_IOPORT_PinRead(&g_ioport_ctrl, I2C1_SDA_PIN, &level);
    return level == BSP_IO_LEVEL_HIGH;
}

void i2c1_bb_take_over(void) {
    scl_drive(true);
    sda_drive(true);
}

void i2c1_bb_release(void) {
    R_IOPORT_PinCfg(&g_ioport_ctrl, I2C1_SCL_PIN,
        (uint32_t)IOPORT_PERIPHERAL_IIC | (uint32_t)IOPORT_CFG_PERIPHERAL_PIN);
    R_IOPORT_PinCfg(&g_ioport_ctrl, I2C1_SDA_PIN,
        (uint32_t)IOPORT_PERIPHERAL_IIC | (uint32_t)IOPORT_CFG_PERIPHERAL_PIN);
}

void i2c1_bb_start(void) {
    sda_drive(true);  scl_drive(true);  i2c1_bb_delay();
    sda_drive(false); i2c1_bb_delay();   // SDA falls while SCL high = START
    scl_drive(false); i2c1_bb_delay();
}

void i2c1_bb_stop(void) {
    sda_drive(false); scl_drive(false); i2c1_bb_delay();
    scl_drive(true);  i2c1_bb_delay();
    sda_drive(true);  i2c1_bb_delay();   // SDA rises while SCL high = STOP
}

void i2c1_bb_clock_bit(bool bit) {
    sda_drive(bit);
    i2c1_bb_delay();
    scl_drive(true);
    i2c1_bb_delay();
    scl_drive(false);
}

bool i2c1_bb_clock_ack(void) {
    sda_drive(true);   // release for the slave to drive
    i2c1_bb_delay();
    scl_drive(true);
    i2c1_bb_delay();
    bool acked = !sda_read();   // ACK = slave pulls SDA low
    scl_drive(false);
    return acked;
}

bool i2c1_bb_write_byte(uint8_t b) {
    for (int bit = 7; bit >= 0; bit--) {
        i2c1_bb_clock_bit((b >> bit) & 1U);
    }
    return i2c1_bb_clock_ack();
}

void i2c1_bb_recover(void) {
    i2c1_bb_take_over();
    sda_drive(true);
    if (sda_read()) {
        i2c1_bb_release();   // SDA already free — nothing stuck
        return;
    }
    for (int i = 0; i < 9 && !sda_read(); i++) {
        scl_drive(true);  i2c1_bb_delay();
        scl_drive(false); i2c1_bb_delay();
    }
    i2c1_bb_stop();
    i2c1_bb_release();
}
