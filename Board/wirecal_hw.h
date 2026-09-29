// Board/wirecal_hw.h — Nano measurement shim for Robot/limbs/wirecal.

#ifndef BOARD_WIRECAL_HW_H
#define BOARD_WIRECAL_HW_H

// wirecal_hw_init — bind the sense-tap/VIN measurement and start the wire
// calibration module. Call after muscle_sr_init() and init_tea().
void wirecal_hw_init(void);

#endif
