// Board/i2c1.h — IIC1 master driver (SCL=P100, SDA=P101), 400 kHz Fast-mode.
//
// This is the limb-array bus: 4x ADS1015 (0x48-0x4B), 4x MCP4728
// (0x60-0x63 after one-time assignment, see mcp4728_addr_assign.h), and
// one MPU6050 (0x68). Separate from any other IIC bus on this board.
//
// Fixed at 400 kHz even though this MCU's IIC peripheral can run Fast-mode
// Plus (1 MHz): the MPU-6050 on this bus is Fast-mode limited (400 kHz
// max per its datasheet), and I2C is a shared-clock bus — every device
// runs at the slowest common rate.
//
// Blocking transaction API, like the driver this replaces: each call is a
// full START..STOP transaction. On failure (NACK, arbitration loss, or a
// bounded-spin timeout), it runs a bit-banged bus-recovery sequence
// (see i2c1_bitbang.h) and retries the transaction once before giving up.

#ifndef BOARD_I2C1_H
#define BOARD_I2C1_H

#include <stdint.h>
#include <stdbool.h>

// i2c1_init — open the IIC1 master instance. Call once from hal_entry
// before any i2c1_write/i2c1_read call.
void i2c1_init(void);

// i2c1_write — write len bytes to the 7-bit address addr7. Returns false
// if the transaction (and one recovery+retry) both fail.
bool i2c1_write(uint8_t addr7, const uint8_t *data, uint32_t len);

// i2c1_read — read len bytes from the 7-bit address addr7. Returns false
// if the transaction (and one recovery+retry) both fail.
bool i2c1_read(uint8_t addr7, uint8_t *data, uint32_t len);

#endif // BOARD_I2C1_H
