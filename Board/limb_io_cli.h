#ifndef BOARD_LIMB_IO_CLI_H
#define BOARD_LIMB_IO_CLI_H

// Board/limb_io_cli.h — CLI word backends for Board/limb_io. Bound to CLI
// text commands via Board/boardwords.txt -> wordlist.c.

void limb_io_cli_status(void);      // show all 16 ADC/DAC channels, IMU, per-limb I2C health
void limb_io_cli_dac_set(void);     // (code ch --) write raw 12-bit code (0-4095) to DAC channel ch (0-15)
void limb_io_cli_assign(void);      // one-time: program the 4 MCP4728s to 0x60-0x63 via their SEL lines
void limb_io_cli_assign_one(void);  // (limb --) assign a single limb's MCP4728, for bench debugging
void limb_io_cli_reset_one(void);   // (limb --) move a limb's MCP4728 back to the shared default address 0x60
void i2c1_cli_scan(void);           // probe every address 0x08-0x77 on IIC1, print the ones that ACK
void limb_io_cli_isolated_read(void); // (limb count --) pause the sweep, read limb's channel 0 count times back-to-back with no other bus traffic, print each
void limb_io_cli_debug_ch0(void);   // (limb --) pause the sweep, print the ADS1015's raw Config + Conversion registers around a channel-0 conversion

#endif // BOARD_LIMB_IO_CLI_H
