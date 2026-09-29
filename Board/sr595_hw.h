// Board/sr595_hw.h — Nano R4 pin shim for the Robot/io/sr595 driver.

#ifndef BOARD_SR595_HW_H
#define BOARD_SR595_HW_H

// sr595_hw_init — configure SER/SRCLK/RCLK as low outputs, bind them to
// the sr595 driver and init a chain of `chips` registers. Call from
// hal_entry after the peripheral opens. Returns false on a bad chip count.
#include <stdbool.h>
#include <stdint.h>
bool sr595_hw_init(uint8_t chips);

// sr595_hw_print_pins — print the level read back on SER, SRCLK and RCLK.
void sr595_hw_print_pins(void);

#endif // BOARD_SR595_HW_H
