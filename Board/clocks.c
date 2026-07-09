// clocks.c — Arduino Nano R4 (Renesas RA4M1) board-specific clock code.
//
// TimbreOS/clocks.c provides the board-agnostic utilities:
//   timestamp_to_utc, epoch_to_tm, tm_to_epoch, over_due, micro_sleep,
//   print_build_banner, show_timer.
//
// CLOCK_HAS_BLINK, CLOCK_HAS_INIT, CLOCK_HAS_TICKS, CLOCK_HAS_DELTA are
// defined in project_defs.h, so this file owns all hardware clock functions.
//
// ─────────────────────────────────────────────────────────────────────────────
// CLOCK ARCHITECTURE
// ─────────────────────────────────────────────────────────────────────────────
// Two GPT channels plus the on-chip RTC:
//
//   tick_timer  — GPT0, 32-bit free-running uptime counter.
//                 PCLKD/1024 @ 32 MHz → 31.25 kHz → 32 µs/tick.
//                 Period = 0x7FFFFFFF (rollover ≈ 38 hours).
//                 Feeds get_ticks() for the TEA scheduler.
//   delta_timer — GPT2, 16-bit one-shot alarm used by set_delta_alarm().
//                 PCLKD/1024 @ 32 MHz, same 32 µs tick units.
//                 Max one-shot period ≈ 2.097 s (0xFFFF ticks).
//   g_rtc0      — IRTC running off LOCO (internal ~32 kHz RC oscillator).
//                 Nano R4 has NO 32.768 kHz sub-clock crystal — only the
//                 16 MHz main XO — so LOCO is the only RTC source.
//                 Exposed via get_utc() → tick_get_utc().
//
// ─────────────────────────────────────────────────────────────────────────────
// BRING-UP NOTES (read before touching GPT/ELC config)
// ─────────────────────────────────────────────────────────────────────────────
//
// 1. ONE_SECOND is 31250 (project_defs.h), matching PCLKD/1024 at 32 MHz.
//
// 2. RA4M1 GPT_SOURCE_GPT_A … GPT_SOURCE_GPT_H are fixed ELC slots, not
//    arbitrary channel assignments. Cascading GPT3 → GPT0/GPT2 via
//    GPT_SOURCE_GPT_A left counters stuck at zero. Use source_div prescaler.
//
// 3. Arduino DFU bootloader hands control with PRIMASK=1. FSP SystemInit()
//    does NOT clear it. Explicit __enable_irq() is in hal_entry.c.
//
// 4. RAM-copy cfg overrides for source_div keep RASC regeneration safe;
//    R_GPT_Open stores a pointer into the cfg, so `static` is mandatory.
//
// 5. memory_regions.ld is regenerated for a bare chip at 0x0; DFU bootloader
//    owns 0x0..0x3FFF. memory_regions_custom.ld is used instead (gitignored).

#include "hal_data.h"
#include "r_ioport.h"

#include "tea.h"
#include "printers.h"
#include "cli.h"
#include "clocks.h"
#include "project_defs.h"
#include <time.h>
#include <stdbool.h>

extern const char build_timestamp[];

// Nano R4 user LED — yellow, on port 2 pin 4 (P204), configured in RASC.
#define LED_PIN  BSP_IO_PORT_02_PIN_04

void delta_timer_cb(timer_callback_args_t * p_args);

// GPT2 (delta_timer) is 16-bit — clamp alarm periods to 0xFFFF ticks.
// At PCLKD/1024 = 31.25 kHz that is ~2.097 s max one-shot.
#define DELTA_MAX_TICKS  0xFFFFu

// ── Hardware RTC (IRTC on LOCO) ──────────────────────────────────────────
//
// epoch_to_tm / tm_to_epoch are provided by TimbreOS/clocks.c.
// set_utc() and tick_get_utc() use FSP R_RTC_* API.
//
// *** FSP QUIRK (r_rtc.c FSP 6.2.0): R_RTC_Open() does NOT call
// R_BSP_MODULE_START(FSP_IP_RTC, 0). On RA4M1, MSTPCRD[23] = 1 after reset
// (module stopped), so RCRn writes are silently dropped and the
// FSP_HARDWARE_REGISTER_WAIT inside r_rtc_set_clock_source() spins forever.
// Un-MSTP the RTC before calling R_RTC_Open(). ***

void set_utc(Long utc) {
    struct tm t;
    epoch_to_tm(utc, &t);
    R_RTC_CalendarTimeSet(&g_rtc0_ctrl, &t);
}

Long tick_get_utc(void) {
    struct tm t;
    if (R_RTC_CalendarTimeGet(&g_rtc0_ctrl, &t) != FSP_SUCCESS) return 0;
    return tm_to_epoch(&t);
}

// ── Delta alarm (one-shot GPT2) ──────────────────────────────────────────

void set_delta_alarm(Long t) {
    if ((uint32_t)t > DELTA_MAX_TICKS) { print("#"); t = (Long)DELTA_MAX_TICKS; }
    if (t < 1) t = 1;
    R_GPT_PeriodSet(&delta_timer_ctrl, (uint32_t)t);
    R_GPT_Reset    (&delta_timer_ctrl);
    R_GPT_Start    (&delta_timer_ctrl);   // one-shot — stops itself at TRG
}

void delta_alarm(void) { now(*alarmEvent); }

// FSP callback — name matches RASC's GPT2 p_callback field in hal_data.c.
void delta_timer_cb(timer_callback_args_t * p_args) {
    if (p_args->event == TIMER_EVENT_CYCLE_END) delta_alarm();
}

// ── Uptime in 32 µs ticks ────────────────────────────────────────────────

Long get_ticks(void) {
    timer_status_t s;
    R_GPT_StatusGet(&tick_timer_ctrl, &s);
    return (Long)s.counter;
}

// ── LED heartbeat ─────────────────────────────────────────────────────────
// Yellow LED on P204. Double-blink: short-on · gap · short-on · long-gap.

#define ON_TIME 2
static inline void led_on (void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH); }
static inline void led_off(void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);  }

void blink_leds(void) {
    Long t;
    static enum { GREEN1, SPACE1, GREEN2, SPACE2 } color = SPACE2;
    switch (color) {
    case GREEN1: led_on();  color = SPACE1; t = ON_TIME;       break;
    case SPACE1: led_off(); color = GREEN2; t = 200 - ON_TIME; break;
    case GREEN2: led_on();  color = SPACE2; t = ON_TIME;       break;
    default:
    case SPACE2: led_off(); color = GREEN1; t = 800 - ON_TIME; break;
    }
    in(msec(t), blink_leds);
}

// ── init_clocks ───────────────────────────────────────────────────────────
//
// RTC_BRINGUP_STAGE lets us bisect the RTC hang without losing the CLI.
// Each stage is strictly additive:
//   0 — no RTC init
//   1 — un-MSTP the RTC only
//   2 — add R_RTC_Open()
//   3 — add R_RTC_CalendarTimeGet()
//   4 — add R_RTC_CalendarTimeSet() (full bring-up)
#ifndef RTC_BRINGUP_STAGE
#define RTC_BRINGUP_STAGE 4
#endif

void init_clocks(void) {
    // DWT cycle counter enabled in hal_entry.c before init_clocks() runs.
    never(alarmEvent);

    // ── GPT bring-up (RAM-copy cfg override) ─────────────────────────────
    // RASC picks source_div automatically; we force /1024 for both timers
    // so ONE_SECOND=31250 stays correct. `static` required — R_GPT_Open
    // stores a pointer into the cfg for the timer's lifetime.

    static timer_cfg_t tick_cfg;
    tick_cfg = tick_timer_cfg;
    tick_cfg.source_div        = (timer_source_div_t)10;   /* /1024 */
    tick_cfg.period_counts     = 0x7FFFFFFFu;
    tick_cfg.duty_cycle_counts = 0x3FFFFFFFu;
    R_GPT_Open  (&tick_timer_ctrl, &tick_cfg);
    R_GPT_Start (&tick_timer_ctrl);

    static timer_cfg_t delta_cfg;
    delta_cfg = delta_timer_cfg;
    delta_cfg.source_div        = (timer_source_div_t)10;
    delta_cfg.period_counts     = 0xFFFFu;
    delta_cfg.duty_cycle_counts = 0x7FFFu;
    R_GPT_Open  (&delta_timer_ctrl, &delta_cfg);

    later(blink_leds);
    namedAction(blink_leds);

    // ── Hardware RTC bring-up ────────────────────────────────────────────
#if RTC_BRINGUP_STAGE >= 1
    R_BSP_MODULE_START(FSP_IP_RTC, 0);
#endif
#if RTC_BRINGUP_STAGE >= 2
    (void)R_RTC_Open(&g_rtc0_ctrl, &g_rtc0_cfg);
#endif
#if RTC_BRINGUP_STAGE >= 3
    Long build_utc = timestamp_to_utc(build_timestamp);
    bool seed = true;
    struct tm rtc_now;
    if (R_RTC_CalendarTimeGet(&g_rtc0_ctrl, &rtc_now) == FSP_SUCCESS) {
        if (rtc_now.tm_year >= 120 && tm_to_epoch(&rtc_now) >= build_utc)
            seed = false;
    }
#endif
#if RTC_BRINGUP_STAGE >= 4
    if (seed) set_utc(build_utc);
#endif
    // Do not print() here — transport is not yet initialised; see hal_entry.c.
}
