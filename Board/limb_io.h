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

// limb_io_reset_one — undo assignment: move limb's MCP4728 from its
// assigned address back to the shared factory default (0x60), so it can
// be freely reassigned afterward. No-op-equivalent for limb 0 (already at
// 0x60).
bool limb_io_reset_one(uint8_t limb);

// limb_io_pause_sweep / limb_io_resume_sweep — stop/restart the periodic
// sweep. Used to get a clean bus (no IMU reads, no other limbs) for the
// diagnostics below; also safe to call standalone.
void limb_io_pause_sweep(void);
void limb_io_resume_sweep(void);

// limb_io_isolated_read — diagnostic: pauses the sweep, then reads
// channel (0-3) of limb (0-3) `count` times back-to-back (same address,
// same channel, no other bus traffic in between — unlike the normal
// sweep, which always precedes a limb's first-scanned channel with a
// different device), printing each raw code, then resumes the sweep. See
// Board/limb_io_cli.c for the `i2ctest` CLI word. count is clamped to
// 1-50.
void limb_io_isolated_read(uint8_t limb, uint8_t channel, uint8_t count);

// limb_io_debug_ch0 — diagnostic: pauses the sweep, then prints the
// ADS1015's raw Config register (not just the downstream conversion
// result) before starting a channel-0 conversion, immediately after
// starting it, and after waiting out the conversion time — plus the final
// conversion register value. Lets you see whether the OS/MUX/PGA/MODE/DR
// write actually landed as intended, instead of only ever seeing "code=0"
// and having to guess why. See Board/limb_io_cli.c for the `i2cregs` CLI
// word.
void limb_io_debug_ch0(uint8_t limb);

// limb_io_test_sequence — diagnostic: pauses the sweep, reads the IMU
// (to reliably reproduce the "just switched I2C target" scenario the
// normal sweep always hits), then reads all 4 of limb's ADC channels
// back-to-back starting from start_channel (wrapping: e.g. start_channel
// 2 -> order 2,3,0,1), printing each. Answers "is address-switch-adjacent
// staleness specific to channel 0, or to whichever channel happens to be
// first in the sequence" — run with start_channel 0,1,2,3 in turn and
// compare which position (not which channel number) looks wrong against
// known DAC values. See Board/limb_io_cli.c for the `i2cseq` CLI word.
void limb_io_test_sequence(uint8_t limb, uint8_t start_channel);

// limb_io_verify — self-test: for each of limb's 4 ADC channels, prints
// the current sweep-cached value (what limbio shows) side by side with a
// freshly-taken isolated reading (ground truth), flagging any pair that
// differs by more than a small noise tolerance. Run any time to check
// whether the sweep's cache still matches reality — not just for the
// known channel-0-position issue, but as a standing regression check for
// any other staleness this hasn't caught yet. See Board/limb_io_cli.c for
// the `i2cverify` CLI word.
void limb_io_verify(uint8_t limb);

// limb_io_test_harness — automated pass/fail regression test: writes all 4
// of limb's DACs to zero (expect ADCs ~0), then to 1000/2000/3000/4000
// (expect ADC[ch] proportional to ADC[ch0]*(ch+1)), then back to zero
// (expect ADCs ~0 again) — each phase's reads start from start_channel
// (wrapping) and are IMU-primed, same as limb_io_test_sequence. Prints a
// PASS/FAIL per channel per phase plus a final N/3 summary. Run with
// start_channel 0-3 across separate calls to check whether any staleness
// is tied to channel 0 specifically or to whichever channel reads first.
// See Board/limb_io_cli.c for the `i2charness` CLI word.
void limb_io_test_harness(uint8_t limb, uint8_t start_channel);

// limb_io_ready — fired once per completed sweep (all 16 ADC channels +
// IMU). Nothing in this module calls when() on it; it's the extension
// seam for a future planning/gait layer.
extern Event limb_io_ready;

#endif // BOARD_LIMB_IO_H
