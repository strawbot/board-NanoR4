// Board/mcp4728_addr_assign.h — one-time MCP4728 I2C address provisioning.
//
// All 4 MCP4728 DACs on this board boot at the same default address
// (0x60) and need their address bits reprogrammed into EEPROM before
// Robot/io/mcp4728 can talk to them individually. Per datasheet DS22187E
// Fig. 5-11, that command requires the target chip's SEL/LDAC pin to
// transition high->low at a specific clock edge inside the transaction
// (finer-grained than the FSP r_iic_master driver's byte-level interrupt
// can hit), so this bit-bangs the whole command manually on the IIC1 pins
// via Board/i2c1_bitbang.h. See that header for why this needs raw pins
// at all.
//
// EEPROM retains the address across power cycles, so this only needs to
// run once per board after assembly — not on every boot.

#ifndef BOARD_MCP4728_ADDR_ASSIGN_H
#define BOARD_MCP4728_ADDR_ASSIGN_H

#include "hal_data.h"
#include <stdint.h>
#include <stdbool.h>

// mcp4728_addr_assign_one — reprogram the MCP4728 currently sitting at
// current_addr7 whose SEL/LDAC pin is sel_pin, so it becomes target_addr7
// instead. Any other MCP4728 also still at current_addr7 on the bus is
// unaffected (see mcp4728_addr_assign.c for why: only the chip whose SEL
// pin sees the required edge transition ever ACKs the critical 3rd byte).
// Safe to call with target_addr7 == current_addr7 (a no-op
// address-to-itself, used to make a uniform 4-chip provisioning loop
// idempotent). Takes over and releases the IIC1 pins itself (see
// i2c1_bitbang.h) — do not call while any other IIC1 transaction could be
// in flight.
//
// current_addr7 is normally 0x60 (the factory default all 4 chips share
// before first assignment), but pass whatever the chip is actually at now
// to reprogram an already-assigned one — e.g. 0x60 as the *target* to
// move a chip back to the shared default (see limb_io_reset_one()).
//
// Returns true if the target chip ACKed the address-change byte (byte 3);
// false means it didn't take (wrong current address, no chip there, or
// the SEL-pin timing wasn't recognized — see the risk note in the
// project's I2C limb-array plan).
bool mcp4728_addr_assign_one(bsp_io_port_pin_t sel_pin, uint8_t current_addr7, uint8_t target_addr7);

#endif // BOARD_MCP4728_ADDR_ASSIGN_H
