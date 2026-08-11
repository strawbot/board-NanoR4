// Board/limb_io.h — 4-limb ADC/DAC array + IMU orchestration.
//
// This board has 4 limbs, each with its own ADS1015 (4-channel ADC, force/
// position feedback) and MCP4728 (4-channel DAC, tendon actuation), plus
// one shared MPU6050 (6-DOF IMU, body orientation) — see the project's I2C
// limb-array plan for the full hardware map. This file owns the topology
// (which chip is where, which SEL line belongs to which MCP4728) and runs
// a non-blocking periodic sweep that keeps a snapshot of every ADC channel
// and the IMU fresh, plus a passthrough for writing DAC channels.
//
// A future planning/gait layer is expected to sit on top of this: read
// limb_io_get_adc()/limb_io_get_imu(), decide new DAC values, call
// limb_io_set_dac(). when(limb_io_ready, ...) is the hook for reacting to
// each freshly completed sweep instead of polling.

#ifndef BOARD_LIMB_IO_H
#define BOARD_LIMB_IO_H

#include "tea.h"
#include "mpu6050.h"
#include <stdint.h>
#include <stdbool.h>

#define LIMB_IO_LIMBS        4U
#define LIMB_IO_CHANNELS     16U   // LIMB_IO_LIMBS * 4

// limb_io_init — bind the chip drivers to i2c1, wake the IMU, and arm the
// first sweep tick. Call once from hal_entry, after i2c1_init() and after
// init_tea() (the sweep is scheduled via in(), which needs the scheduler
// running).
void limb_io_init(void);

// limb_io_set_dac — write raw 12-bit code (0-4095) to DAC channel ch
// (0-15 = limb*4 + sub_channel). Immediate — does not wait for the sweep.
// Returns false on an out-of-range channel or I2C failure.
bool limb_io_set_dac(uint8_t ch, uint16_t code12);

// limb_io_get_adc — last-swept raw 12-bit ADC code for channel ch (0-15).
// Stale (not "not yet read") data reads back as 0 before the first sweep
// completes.
uint16_t limb_io_get_adc(uint8_t ch);

// limb_io_get_dac — last code written by limb_io_set_dac() for channel ch
// (0-15). Firmware-side shadow, not a fresh chip read-back.
uint16_t limb_io_get_dac(uint8_t ch);

// limb_io_get_imu — last-swept IMU reading. Returns false if no sweep has
// completed an IMU read yet.
bool limb_io_get_imu(mpu6050_data_t *out);

// limb_io_adc_fail_count / limb_io_dac_fail_count — consecutive I2C
// failure count for limb (0-3)'s ADS1015 / MCP4728, reset to 0 on the
// next success. limb_io_imu_fail_count for the shared MPU6050.
uint8_t limb_io_adc_fail_count(uint8_t limb);
uint8_t limb_io_dac_fail_count(uint8_t limb);
uint8_t limb_io_imu_fail_count(void);

// limb_io_assign_addresses — one-time MCP4728 EEPROM address provisioning
// for all 4 limbs (see Board/mcp4728_addr_assign.h). Blocks for the
// duration of the bit-banged sequence (a few hundred microseconds per
// chip) — this is a bench/setup operation, not something the sweep or a
// planning layer calls at runtime.
bool limb_io_assign_addresses(void);

// limb_io_assign_one — same, for a single limb (0-3), for bench debugging
// one chip at a time.
bool limb_io_assign_one(uint8_t limb);

// limb_io_ready — fired once per completed sweep (all 16 ADC channels +
// IMU). Nothing in this module calls when() on it; it's the extension
// seam for a future planning/gait layer.
extern Event limb_io_ready;

#endif // BOARD_LIMB_IO_H
