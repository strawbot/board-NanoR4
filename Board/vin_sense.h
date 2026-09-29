// Board/vin_sense.h — supply voltage from the VIN_S divider on A3 (AN002).
//
// VIN -- R_TOP (100K, 0.1 %) -- tap (A3) -- R_BOT (47K nominal, 5 %) -- GND.
// R_BOT's tolerance is the largest error in the reading, so it is calibrated
// against a meter (`vin-cal`) and saved in the calibration file (Robot/
// storage/calib) as `rbot!`. Because the calibration is solved from the ADC's
// own reading, it also absorbs the ADC gain error at the tap.

#ifndef BOARD_VIN_SENSE_H
#define BOARD_VIN_SENSE_H

#include <stdint.h>

void     vin_sense_init(void);      // register the calibration value; call once at boot
uint32_t vin_tap_mv(void);          // voltage at the A3 pin, mV
uint32_t vin_read_mv(void);         // supply voltage through the calibrated divider, mV
uint32_t vin_rbot_ohms(void);       // R_BOT in use

// CLI words
void vin_cli_read(void);            // ( -- mv )  vin@
void vin_cli_cal(void);             // ( mv -- )  vin-cal: solve R_BOT from a metered VIN, save it
void vin_cli_rbot_set(void);        // ( ohms -- ) rbot!: set R_BOT directly (the calib file runs this)

#endif
