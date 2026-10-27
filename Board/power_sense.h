// Board/power_sense.h — closed-loop power sensing: FET drain voltage, raw
// sense-resistor voltage, and the wire resistance computed from them plus
// VIN (Board/vin_sense.h): R_wire = (VIN - V_drain) / (V_sense / R_sense).
//
// V_drain -- R_TOP (10K, 5%) -- tap (P102/AN020) -- R_BOT (2.2K, 5%) -- GND.
//            Low-value 5% pair, not 0.1% — chosen for low Thevenin impedance
//            (~1.8K) so the ADC's own repeated sampling doesn't droop the
//            node (confirmed a real effect on the bench at the original
//            82.5K/22K pair). The 5% tolerance is absorbed by vdrain-cal
//            against a meter, same as vin_sense.c's own rbot!/vin-cal.
// V_sense -- direct to P103/AN019, no divider (already well inside ADC range
//            with either sense-resistor option below).
//
// The sense resistor's own value is a compile-time constant in power_sense.c
// — update it there if the installed part changes (1R 1/4W was used for
// every bench characterization so far; 0R5 2W is the higher-current-headroom
// option for testing above the 1R part's ~500mA rating). Getting this wrong
// silently scales every current and resistance figure this module reports.

#ifndef BOARD_POWER_SENSE_H
#define BOARD_POWER_SENSE_H

#include <stdint.h>

void     power_sense_init(void);    // register vdbot!'s calibration value; call once at boot
uint32_t vdrain_tap_mv(void);       // voltage at the P102 pin, mV
uint32_t vdrain_mv_from_tap(uint32_t tap_mv);  // pure math, no ADC access — Vdrain from an already-read tap mV
uint32_t vdrain_read_mv(void);      // FET drain voltage through the divider, mV
uint32_t vdrain_rbot_ohms(void);    // R_BOT in use for the Vdrain divider
uint32_t vsense_read_mv(void);      // raw sense-resistor voltage, mV (direct, no divider)
uint32_t isense_ma_from_vsense(uint32_t vsense_mv);  // pure math, no ADC access
uint32_t isense_read_ma(void);      // sense current, mA (from vsense_read_mv() and R_SENSE_MOHM)
uint32_t rwire_mohm_from(uint32_t vin_mv, uint32_t vdrain_mv, uint32_t vsense_mv);  // pure math, no ADC access
uint32_t rwire_read_mohm(void);     // wire resistance, milliohms — 0 if no current flowing (can't compute)

// CLI words
void power_cli_vdrain(void);        // ( -- mv )    vdrain@
void power_cli_vsense(void);        // ( -- mv )    vsense@
void power_cli_isense(void);        // ( -- ma )    isense@
void power_cli_rwire(void);         // ( -- mohm )  rwire@
void power_cli_vdbot_set(void);     // ( ohms -- )  vdbot!: set the Vdrain divider's R_BOT directly (the calib file runs this)
void power_cli_vdrain_cal(void);    // ( mv -- )    vdrain-cal: solve R_BOT from a metered Vdrain, save it
void power_cli_status(void);        // print VIN/V_drain/V_sense (raw + mV) and I_sense/R_wire — pstatus

#endif
