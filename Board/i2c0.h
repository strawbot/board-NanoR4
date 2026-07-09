// Board/i2c0.h — IIC0 master driver (SCL=P400, SDA=P401), 400 kHz Fast-mode.
//
// Blocking transaction API. Each call is a full START..STOP transaction —
// no repeated-start combined write/read primitive is exposed because none
// of the boards on this bus need one (see Board/i2c0.c for details).

#ifndef BOARD_I2C0_H
#define BOARD_I2C0_H

#include <stdint.h>
#include <stdbool.h>

// i2c0_init — open the IIC0 master instance. Call once from hal_entry
// before any i2c0_write/i2c0_read call.
void i2c0_init(void);

// i2c0_write — write len bytes to the 7-bit address addr7. Returns false
// on NACK, arbitration loss, or timeout.
bool i2c0_write(uint8_t addr7, const uint8_t *data, uint32_t len);

// i2c0_read — read len bytes from the 7-bit address addr7. Returns false
// on NACK, arbitration loss, or timeout.
bool i2c0_read(uint8_t addr7, uint8_t *data, uint32_t len);

#endif // BOARD_I2C0_H
