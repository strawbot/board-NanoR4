// board_cli.c — Nano R4 (RA4M1) CLI command backends.
//
// MVP port: STM32 LL_RCC/LL_TIM/RCC_APBxENR machinery stripped out; show_sys
// and show_timers emit human-readable placeholders so the CLI commands stay
// wired up. Real FSP-based clock/timer surveys will land once the r_cgc stack
// is opened and the GPT+AGT peripherals have accessors on this MCU.

#include <stdbool.h>
#include <stdint.h>

#include "hal_data.h"
#include "tea.h"
#include "printers.h"
#include "project_defs.h"
#include "clocks.h"
#include "board_cli.h"

// ── clocks & uptime ────────────────────────────────────────────────────────
void show_sys(void) {
    // Clock tree is pinned down in ra_gen/bsp_clock_cfg.h.
    // XTAL 16 MHz → PLL ×8 /2 = 64 MHz → ICLK /2 = 32 MHz; PCLKD /2 = 32 MHz.
    print("SYSCLK:  "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("ICLK:    "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("PCLKD:   "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("uptime:  "); printDec(get_ticks()); print(" ticks (100 us)"); printCr();
    show_timer();
    printCr();
}

// ── timer survey ───────────────────────────────────────────────────────────
// STUB: real GPT survey deferred. The RASC-opened timers are:
//   prescalar   — GPT3  100 us periodic (count source for the others)
//   tick_timer  — GPT0  32-bit free-running uptime
//   delta_timer — GPT2  one-shot alarm (16-bit)
void show_timers(void) {
    print("timers:"); printCr();
    print("  prescalar   GPT3  periodic  (100 us tick)"); printCr();
    print("  tick_timer  GPT0  up counter "); printDec(get_ticks()); printCr();
    print("  delta_timer GPT2  one-shot alarm"); printCr();
    print_RTC();
    printCr();
}

// ── reboot ─────────────────────────────────────────────────────────────────
void do_reboot(void) {
    print("rebooting..."); printCr();
    NVIC_SystemReset();
}

// ── RTC ────────────────────────────────────────────────────────────────────
// STUB: RTC integration deferred. Replace with R_RTC_CalendarTimeGet(&g_rtc0_ctrl, &t)
// and BCD-free formatting once the r_rtc stack is opened in hal_entry.
void print_RTC(void) {
    print("RTC: --");
}

// ── gpio_dump_all stub ─────────────────────────────────────────────────────
// gpio_dump.c is still STM32-only and excluded from the build. Provide a
// placeholder so the CLI word `pins` resolves.
void gpio_dump_all(void) {
    print("pins: gpio_dump not ported yet"); printCr();
}

// ── word filter ───────────────────────────────────────────────────────────
bool visible_word(char *s) { (void)s; return true; }
