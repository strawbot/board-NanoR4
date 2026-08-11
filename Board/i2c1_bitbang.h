// Board/i2c1_bitbang.h — manual GPIO clocking of the IIC1 pins (P100=SCL,
// P101=SDA), shared by two things that need control the FSP r_iic_master
// peripheral driver can't give:
//
//   1. Board/i2c1.c bus recovery — a slave stuck holding SDA low needs up
//      to 9 manual SCL pulses + a STOP condition to let go, which has to
//      happen on the raw pins since the hardware IIC unit that's stuck is
//      the thing we're trying to un-stick.
//   2. Board/mcp4728_addr_assign.c — the MCP4728 address-write command
//      needs a select-pin transition synchronized to a specific clock
//      edge inside one byte, finer-grained than the FSP driver's
//      byte-level TXI interrupt can hit.
//
// Not for routine transactions — Board/i2c1.c uses the hardware IIC1
// peripheral for those. This bit-bangs at ~50 kHz (see i2c1_bb_delay()),
// well under Fast-mode, for generous timing margin on both use cases.

#ifndef BOARD_I2C1_BITBANG_H
#define BOARD_I2C1_BITBANG_H

#include <stdint.h>
#include <stdbool.h>

// i2c1_bb_take_over / i2c1_bb_release — switch P100/P101 between plain
// GPIO (for bit-banging) and the IIC1 peripheral function. Must bracket
// every bit-bang session; the IIC1 hardware instance (Board/i2c1.c) must
// not be used while GPIO mode is active.
void i2c1_bb_take_over(void);
void i2c1_bb_release(void);

// Half-bit-period delay at the bit-banged rate (~50 kHz -> ~10us/half-bit).
void i2c1_bb_delay(void);

void i2c1_bb_start(void);   // START condition
void i2c1_bb_stop(void);    // STOP condition

// i2c1_bb_clock_bit — drive SDA to `bit` and pulse SCL once (one data
// bit). Caller sets SDA before rising SCL and holds it through the high
// phase, matching normal I2C data timing.
void i2c1_bb_clock_bit(bool bit);

// i2c1_bb_clock_ack — release SDA (let the slave drive it) and pulse SCL
// for the ACK/NACK bit; returns true if the slave pulled SDA low (ACK).
bool i2c1_bb_clock_ack(void);

// i2c1_bb_write_byte — clock 8 data bits MSB-first then the ACK bit;
// returns true if ACKed.
bool i2c1_bb_write_byte(uint8_t b);

// i2c1_bb_recover — if a transaction left a slave holding SDA low, clock
// up to 9 SCL pulses watching for SDA to release, then issue STOP. Safe
// to call unconditionally; a no-op if SDA is already high. Takes over and
// releases the pins itself.
void i2c1_bb_recover(void);

#endif // BOARD_I2C1_BITBANG_H
