#ifndef BOARD_LIMB_IO_CLI_H
#define BOARD_LIMB_IO_CLI_H

// Board/limb_io_cli.h — CLI word backends for Board/limb_io. Bound to CLI
// text commands via Board/boardwords.txt -> wordlist.c.

void limb_io_cli_status(void);      // show all 16 ADC/DAC channels, IMU, per-limb I2C health
void limb_io_cli_dac_set(void);     // (code ch --) write raw 12-bit code (0-4095) to DAC channel ch (0-15)
void limb_io_cli_assign(void);      // one-time: program the 4 MCP4728s to 0x60-0x63 via their SEL lines
void limb_io_cli_assign_one(void);  // (limb --) assign a single limb's MCP4728, for bench debugging

#endif // BOARD_LIMB_IO_CLI_H
