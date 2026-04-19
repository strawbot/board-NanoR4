// clocks.c — Nano R4 (RA4M1) port
//
// Time base for TimbreOS on Renesas RA4M1 using FSP GPT peripherals.
//
//   prescalar   — GPT3,  periodic, period = 3200 @ PCLKD 32 MHz → 100 µs ticks
//                 feeds its overflow as count source to tick_timer + delta_timer
//                 (RASC config: count_up_source = GPT_SOURCE_GPT_A).
//   tick_timer  — GPT0,  32-bit free-running uptime counter in 100 µs units.
//                 Clocked off prescalar overflow; ARR left at 0x7fffffff so
//                 a simple counter read gives monotonic ticks.
//   delta_timer — GPT2,  16-bit one-shot alarm used by set_delta_alarm().
//                 Also clocked off prescalar overflow, so its period counts
//                 are in the same 100 µs units as ONE_SECOND.
//
// RTC integration is stubbed (set_utc / print_RTC) until the r_rtc stack is
// brought online.

#include "hal_data.h"
#include "r_ioport.h"

#include "tea.h"
#include "printers.h"
#include "cli.h"
#include "project_defs.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// IOPORT is opened by RASC's hal_warmstart.c before main() runs — we just
// reach into the generated g_ioport_ctrl for PinWrite calls. No R_IOPORT_Open
// here.

// Nano R4 user LED — yellow, on port 2 pin 4 (P204), configured in RASC.
#define LED_PIN  BSP_IO_PORT_02_PIN_04

void print_RTC();
void delta_timer_cb(timer_callback_args_t * p_args);

// 100 µs tick resolution — must match ONE_SECOND in project_defs.h.
// prescalar counts PCLKD cycles; at 32 MHz this gives 32e6 / 10000 = 3200.
#define PRESCALAR_PERIOD_COUNTS  (CLOCK_MHZ * 1000000u / ONE_SECOND)

// GPT2 (delta_timer) is 16-bit — clamp alarm periods to 0xFFFF ticks (6.55 s).
#define DELTA_MAX_TICKS  0xFFFFu

// ── UTC time-of-build helper (unchanged) ────────────────────────────────────
// Parse __TIMESTAMP__ string ("Www Mmm DD HH:MM:SS YYYY") into a UTC Unix timestamp.
static Long timestamp_to_utc(const char *ts) {
    static const char * const mon_names[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    struct tm t = {0};
    char wday[4], mon[4];
    int day, hour, min, sec, year;

    sscanf(ts, "%3s %3s %d %d:%d:%d %d",
           wday, mon, &day, &hour, &min, &sec, &year);

    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    t.tm_year  = year - 1900;
    t.tm_isdst = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, mon_names[i], 3) == 0) { t.tm_mon = i; break; }
    }
    // mktime() on newlib bare-metal treats struct tm as UTC (no tz offset)
    return (Long)mktime(&t);
}

// STUB: RTC integration deferred. Replace with R_RTC_CalendarTimeSet(&g_rtc0_ctrl, ...)
// once the r_rtc stack is opened and we have a Unix-epoch → struct tm helper.
void set_utc(Long utc) {
    (void)utc;
}

void over_due() { /* incCtr(overDueTea); */ }

// ── delta alarm (one-shot GPT2) ─────────────────────────────────────────────
//
// Translate a delta (in 100 µs ticks) into a one-shot GPT2 period and fire.
// GPT2 is 16-bit, so periods > 0xFFFF must be clamped; the tea scheduler will
// re-arm when this shorter alarm fires.
void set_delta_alarm(Long t) {
    if (t > DELTA_MAX_TICKS) {
        print("#");
        t = DELTA_MAX_TICKS;
    }
    if (t < 1) t = 1;
    R_GPT_PeriodSet(&delta_timer_ctrl, (uint32_t)t);
    R_GPT_Reset    (&delta_timer_ctrl);
    R_GPT_Start    (&delta_timer_ctrl);   // one-shot — stops itself at TRG
}

// Called from delta_alarm_cb (FSP callback, see below). Signals the scheduler.
void delta_alarm() {
    now(*alarmEvent);
}

// ── FSP callback trampoline ────────────────────────────────────────────────
//
// Name matches RASC's GPT2 `p_callback` field in ra_gen/hal_data.c.
void delta_timer_cb(timer_callback_args_t * p_args) {
    if (p_args->event == TIMER_EVENT_CYCLE_END) {
        delta_alarm();
    }
}

// ── uptime in 100 µs ticks ─────────────────────────────────────────────────
Long get_ticks() {
    timer_status_t s;
    R_GPT_StatusGet(&tick_timer_ctrl, &s);
    return (Long)s.counter;
}

// ── diagnostic ─────────────────────────────────────────────────────────────
void show_timer() {
    print("  ticks/S:");
    printDec(ONE_SECOND);
    print("  ticks:");
    printDec(get_ticks());
    print("  "), print_RTC();
}

// WFI is portable across Cortex-M — sleeps until any interrupt wakes the core.
void micro_sleep() { __WFI(); }

// ── LED blink ──────────────────────────────────────────────────────────────
// Nano R4 yellow LED on P204. Classic two-flash heartbeat — short-on, short-gap,
// short-on, long-gap.
#define ON_TIME 2
static inline void led_on (void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH); }
static inline void led_off(void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);  }

static void blink_leds() {
    Long t;
    static enum {GREEN1,SPACE1,GREEN2,SPACE2} color = SPACE2;
    switch (color) {
    case GREEN1:  led_on();  color = SPACE1; t = ON_TIME;       break;
    case SPACE1:  led_off(); color = GREEN2; t = 200 - ON_TIME; break;
    case GREEN2:  led_on();  color = SPACE2; t = ON_TIME;       break;
    default:
    case SPACE2:  led_off(); color = GREEN1; t = 800 - ON_TIME; break;
    }
    in(msec(t), blink_leds);
}

// ── init ───────────────────────────────────────────────────────────────────
void init_clocks() {
    // DWT cycle counter was enabled in hal_entry.c; IOPORT was opened by
    // R_BSP_WarmStart() during BSP bring-up, before main().

    never(alarmEvent);

    // prescalar: free-running 100 µs timebase, feeds tick_timer + delta_timer
    // via GPT_SOURCE_GPT_A (configured in RASC).
    R_GPT_Open     (&prescalar_ctrl,   &prescalar_cfg);
    R_GPT_PeriodSet(&prescalar_ctrl,   PRESCALAR_PERIOD_COUNTS);  // 100 µs
    R_GPT_Start    (&prescalar_ctrl);

    // tick_timer: 32-bit free-running counter in 100 µs units (uptime).
    R_GPT_Open  (&tick_timer_ctrl, &tick_timer_cfg);
    R_GPT_Start (&tick_timer_ctrl);

    // delta_timer: one-shot, started on demand by set_delta_alarm().
    R_GPT_Open  (&delta_timer_ctrl, &delta_timer_cfg);

    later(blink_leds);
    namedAction(blink_leds);

    print("\nBuilt: "), print(__TIMESTAMP__);
    set_utc(timestamp_to_utc(__TIMESTAMP__));
}
