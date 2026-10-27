// Board/vin_sense.h — supply voltage from the VIN divider on P101 (AN021).
//
// VIN -- R_TOP (100K, 0.1 %) -- tap (P101/AN021) -- R_BOT (20K, 0.1 %) -- GND.
// Was on AN002/P002 (A3) for the old hex board; that pin is now AMP0O's own
// output (Board/analog_test.c), so this moved to a genuinely free ADC pin
// with a fresh divider. R_BOT is still calibrated against a meter (`vin-cal`)
// and saved in the calibration file (Robot/storage/calib) as `rbot!`, same as
// before — 0.1% parts this time, but worth keeping the calibration path
// since it also absorbs the ADC's own gain error at the tap, not just the
// resistors' tolerance.

#ifndef BOARD_VIN_SENSE_H
#define BOARD_VIN_SENSE_H

#include <stdint.h>

void     vin_sense_init(void);      // register the calibration value; call once at boot
uint32_t vin_tap_mv(void);          // voltage at the A3 pin, mV
uint32_t vin_mv_from_tap(uint32_t tap_mv); // pure math, no ADC access — VIN from an already-read tap mV
uint32_t vin_read_mv(void);         // supply voltage through the calibrated divider, mV
uint32_t vin_rbot_ohms(void);       // R_BOT in use

// CLI words
void vin_cli_read(void);            // ( -- mv )  vin@
void vin_cli_cal(void);             // ( mv -- )  vin-cal: solve R_BOT from a metered VIN, save it
void vin_cli_rbot_set(void);        // ( ohms -- ) rbot!: set R_BOT directly (the calib file runs this)

#endif
