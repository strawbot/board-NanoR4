#ifndef BOARD_CLI_H
#define BOARD_CLI_H

// Nano R4 board CLI command implementations.
// All functions are void(void) as required by the WordLists parser.
// Bound to CLI text commands via Board/boardwords.txt → wordlist.c

// system
void show_sys(void);        // Clock frequencies and uptime
void show_timers(void);     // GPT/AGT/RTC peripheral survey
void do_reboot(void);       // NVIC system reset

// rtc
void print_RTC(void);       // One-line RTC time-of-day ("UTC YYYY-MM-DD HH:MM:SS")
void cli_set_utc(void);     // CLI wrapper for set_utc(): pops Unix epoch off data stack

// gpio
void gpio_dump_all(void);   // Pin/state dump (currently a stub on Nano)

// dac
void dac_init(void);        // Open DAC0, zero output, start (call once at boot)
void dac_set(void);         // CLI word: pops 0-4095 off stack, writes to DAC0/P014

// adc
void adc_init(void);        // Open ADC0 unit 0, configure AN00, start continuous scan (call once at boot)
uint32_t adc_read_mv(void); // Read AN00 (P000) and return millivolts (0–5000), callable any time
void cli_adc_read(void);    // CLI word: pushes AN00 reading in mV onto the data stack

// ad5593r (IIC0: SCL=P400, SDA=P401) — chip driver in Robot/io/ad5593r
void ad5593r_show_status(void);  // print pin config, reference state, and each I/O port's value
void ad5593r_cli_dac_set(void);  // (mv ch) write mv millivolts (0-2500) to DAC channel ch (0-3)
void ad5593r_cli_adc_read(void); // (ch -- mv) read ADC channel ch (0-3), push millivolts
void ad5593r_cli_reset(void);    // reset the chip and reconfigure DAC/ADC/reference

#endif // BOARD_CLI_H
