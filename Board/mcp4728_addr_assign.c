// Board/mcp4728_addr_assign.c — see mcp4728_addr_assign.h for the contract.
//
// Byte layout, datasheet Fig. 5-11 ("Write Command: Write I2C Address
// Bits to the DAC Registers and EEPROM"). Device code is fixed 1100;
// addr7 = 0b1100_A2A1A0, so a 3-bit address value (0-7) is all that
// varies:
//   byte1 (device addressing) = (0x60 | current_bits) << 1 | 0(write)
//   byte2 = 0 1 1 <current_bits:3> 0 1  = 0x60 | (current_bits << 2) | 1
//   byte3 = 0 1 1 <new_bits:3>     1 0  = 0x60 | (new_bits << 2) | 2
//   byte4 = 0 1 1 <new_bits:3>     1 1  = 0x60 | (new_bits << 2) | 3
//
// All 4 MCP4728s on this board start at the shared default (current_bits
// = 0), so byte2 is always 0x61 here.
//
// SEL/LDAC pin timing (Note 2 on the same figure): keep the target chip's
// SEL pin high through byte 2's 8th data bit, drop it low in the gap
// between that bit and byte 2's ACK, hold it low through byte 3's ACK,
// then release it. Any other MCP4728 still sitting at the addressed
// current value ACKs byte 1 and shifts in bytes 2-4 right along with the
// target chip (harmless — open-drain ACK, no conflict) but never ACKs
// byte 3, because its own SEL pin never moved.

#include "r_ioport.h"
#include "mcp4728_addr_assign.h"
#include "i2c1_bitbang.h"

static void sel_set(bsp_io_port_pin_t sel_pin, bool high) {
    R_IOPORT_PinWrite(&g_ioport_ctrl, sel_pin, high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}

static void clock_byte_bits(uint8_t b) {
    for (int bit = 7; bit >= 0; bit--) {
        i2c1_bb_clock_bit((b >> bit) & 1U);
    }
}

bool mcp4728_addr_assign_one(bsp_io_port_pin_t sel_pin, uint8_t target_addr7) {
    uint8_t current_bits = 0x60U & 0x07U;               // all chips start at 0x60
    uint8_t new_bits     = target_addr7 & 0x07U;

    i2c1_bb_take_over();
    R_IOPORT_PinCfg(&g_ioport_ctrl, sel_pin,
        (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t)IOPORT_CFG_PORT_OUTPUT_HIGH);
    i2c1_bb_delay();   // let SEL settle high before byte 2's critical edge

    i2c1_bb_start();
    bool ok = i2c1_bb_write_byte((uint8_t)((0x60U | current_bits) << 1));   // byte1, write

    if (ok) {
        clock_byte_bits((uint8_t)(0x60U | (current_bits << 2) | 0x01U));   // byte2, 8 bits
        sel_set(sel_pin, false);                                            // drop SEL: negative pulse of bit 8, before ACK
        ok = i2c1_bb_clock_ack();
    }

    bool accepted = false;
    if (ok) {
        clock_byte_bits((uint8_t)(0x60U | (new_bits << 2) | 0x02U));       // byte3, 8 bits
        accepted = i2c1_bb_clock_ack();                                    // the datasheet's success signal
        sel_set(sel_pin, true);                                            // release SEL right after byte3's ACK
    } else {
        sel_set(sel_pin, true);
    }

    // Byte 4 (confirmation repeat) is sent regardless, to leave the bus in
    // a well-formed state — the transaction's actual outcome was already
    // decided by byte 3's ACK above.
    clock_byte_bits((uint8_t)(0x60U | (new_bits << 2) | 0x03U));
    i2c1_bb_clock_ack();

    i2c1_bb_stop();
    i2c1_bb_release();
    return accepted;
}
