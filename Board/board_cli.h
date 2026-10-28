#ifndef BOARD_CLI_H
#define BOARD_CLI_H

#include <stdint.h>

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

// gpio — see Board/pins.h for pins/pin@/pindir!/pinset!

// adc
void adc_init(void);        // Open ADC0 unit 0, configure AN00, start continuous scan (call once at boot)
uint16_t adc_read_an_raw(uint8_t an); // Raw 14-bit code of a scanned ANxxx
uint32_t adc_vref_get(void);           // ADC reference in mV used for all conversions
void     adc_cli_vref_set(void);      // CLI word ( mv -- ): fix the ADC reference by hand (tracking off)
void     adc_cli_vref_auto(void);     // CLI word: toggle AVCC0 tracking from the internal reference
void     adc_cli_vint_set(void);      // CLI word ( mv -- ): set the internal reference calibration (used by the calib file)
void     adc_cli_vref_cal(void);      // CLI word ( mv -- ): calibrate the internal reference against a metered AVCC0
void     adc_vref_start(void);        // start AVCC0 tracking (after init_tea)
void     adc_print_vref(void);        // one-line reference status
void     adc_cli_experiment(void);    // CLI word `adcexp`: internal-reference / sensor reading experiment
void     adc_print_diag(void);        // ADC registers, internal reference, die temperature
int      adc_init_status(void);       // First FSP error from adc_init (0 = ok)
uint32_t adc_read_an_mv(uint8_t an); // Any scanned ANxxx (19, 20, 21) in millivolts
void adc_sync_start(void);  // switch to ELC-triggered group-scan (Vsense/Vdrain=group A, VIN=group B), both on GPT1's overflow — call once dac8-cmp's carrier/ELC links are live
void adc_sync_stop(void);   // revert to the original continuous-scan config (adc_init()'s own setup) — call from dac8-stop/analog-test-stop

#endif // BOARD_CLI_H
