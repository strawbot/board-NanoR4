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

#endif // BOARD_CLI_H
