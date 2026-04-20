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
//
// Software-maintained UTC: seeded from __TIMESTAMP__ at boot (see
// Board/clocks.c) and advanced by the tick timer. When the r_rtc stack is
// opened, swap get_utc() / set_utc() to hit the hardware calendar; the
// formatting here is agnostic.
//
// Epoch → YYYY-MM-DD HH:MM:SS using Howard Hinnant's closed-form
// civil_from_days. Zero libc calls (no gmtime_r, no strftime) — avoids the
// newlib time-machinery hang documented in README-RASC.md.

static void print2d(int v) {
    if (v < 10) print("0");
    printDec0(v);
}

static void civil_from_days(long days, int *year, int *month, int *day) {
    days += 719468L;                                     // shift epoch to 0000-03-01
    long era = (days >= 0 ? days : days - 146096L) / 146097L;
    unsigned doe = (unsigned)(days - era * 146097L);     // [0, 146096]
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    long y       = (long)yoe + era * 400L;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp  = (5u * doy + 2u) / 153u;               // [0, 11]
    unsigned d   = doy - (153u * mp + 2u) / 5u + 1u;     // [1, 31]
    unsigned m   = mp < 10u ? mp + 3u : mp - 9u;         // [1, 12]
    *year  = (int)(y + (m <= 2 ? 1 : 0));
    *month = (int)m;
    *day   = (int)d;
}

void print_RTC(void) {
    Long utc  = get_utc();
    long days = (long)utc / 86400L;
    long sod  = (long)utc - days * 86400L;               // seconds-of-day
    if (sod < 0) { sod += 86400L; days -= 1L; }          // handle negative epochs

    int hh = (int)(sod / 3600L);
    int mm = (int)((sod % 3600L) / 60L);
    int ss = (int)(sod % 60L);
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);

    print("UTC ");
    printDec0(y); print("-"); print2d(mo); print("-"); print2d(d);
    print(" ");
    print2d(hh); print(":"); print2d(mm); print(":"); print2d(ss);
}

// ── gpio_dump_all stub ─────────────────────────────────────────────────────
// gpio_dump.c is still STM32-only and excluded from the build. Provide a
// placeholder so the CLI word `pins` resolves.
void gpio_dump_all(void) {
    print("pins: gpio_dump not ported yet"); printCr();
}

// ── word filter ───────────────────────────────────────────────────────────
bool visible_word(char *s) { (void)s; return true; }
