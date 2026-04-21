// clocks.c — Arduino Nano R4 (Renesas RA4M1) port of the TimbreOS time base.
//
// ─────────────────────────────────────────────────────────────────────────────
// CLOCK ARCHITECTURE
// ─────────────────────────────────────────────────────────────────────────────
// Two GPT channels plus the on-chip RTC:
//
//   tick_timer  — GPT0, 32-bit free-running uptime counter.
//                 PCLKD/1024 @ 32 MHz → 31.25 kHz → 32 µs/tick.
//                 Period = 0x7FFFFFFF (rollover ≈ 38 hours).
//                 Feeds sysTicks() / get_ticks() for the TEA scheduler.
//   delta_timer — GPT2, 16-bit one-shot alarm used by set_delta_alarm().
//                 PCLKD/1024 @ 32 MHz, same 32 µs tick units.
//                 Max one-shot period ≈ 2.097 s (0xFFFF ticks);
//                 longer alarms re-arm via the scheduler.
//   g_rtc0      — IRTC running off LOCO (internal ~32 kHz RC oscillator).
//                 Nano R4 has NO 32.768 kHz sub-clock crystal — schematic
//                 only populates the 16 MHz main XO — so LOCO is the only
//                 option for the RTC count source. Drifts ±15% per
//                 datasheet, so wall-clock UTC needs external correction
//                 over long intervals. Holds time across warm resets as
//                 long as MCU power is maintained. Exposed via get_utc()
//                 (see project_defs.h → tick_get_utc below).
//
// ─────────────────────────────────────────────────────────────────────────────
// NOTES ACCUMULATED DURING BRING-UP (read before touching GPT/ELC config)
// ─────────────────────────────────────────────────────────────────────────────
//
// 1. ONE_SECOND is 31250 (see Board/project_defs.h), matching PCLKD/1024 at
//    32 MHz (32 µs/tick). If the board clock tree changes, update both.
//
// 2. On RA4M1 the GPT_SOURCE_GPT_A … GPT_SOURCE_GPT_H enums are *fixed
//    hardware ELC slots*, NOT "pick any GPT channel by letter". An earlier
//    attempt to cascade GPT3 → GPT0/GPT2 via GPT_SOURCE_GPT_A left both
//    downstream counters stuck at zero. If you need a slower tick, use
//    source_div (PCLKD prescaler /1, /2, /4, /8, /16, /32, /64, /256, /1024)
//    — don't try to cascade channels.
//
// 3. The Arduino DFU bootloader hands control to user code with
//    PRIMASK = 1 (IRQs globally masked). FSP's SystemInit() does NOT clear
//    it. Without an explicit __enable_irq() early in hal_entry(), the GPT2
//    alarm ISR never dispatches — the first heartbeat runs (because it
//    fires synchronously off later()), then the scheduler freezes waiting
//    for an IRQ that can't preempt. Fix lives in src/hal_entry.c.
//
// 4. No hand-edits to ra_gen/hal_data.c are required anymore. All RASC-
//    controllable settings (RTC clock source = LOCO, delta_timer mode =
//    ONE_SHOT, IPL = 12, count/clear sources = NONE, etc.) live in the
//    configurator itself. The one field RASC can't pin down reliably — the
//    GPT source divider — is overridden via a RAM-resident cfg copy in
//    init_clocks() below. Regenerate in RASC as freely as you like; nothing
//    on this side needs restoring.
//
//    memory_regions.ld is a different story: RASC regenerates it for a bare
//    chip at 0x0, but the Arduino DFU bootloader owns 0x0..0x3FFF on the
//    Nano R4. To avoid hand-patching after every regen, the linker pulls in
//    memory_regions_custom.ld via script/fsp.ld instead, and the generated
//    memory_regions.ld is gitignored. See Nano/README-RASC.md for details.

#include "hal_data.h"
#include "r_ioport.h"

#include "tea.h"
#include "printers.h"
#include "cli.h"
#include "clocks.h"
#include "project_defs.h"
// <time.h> is included ONLY for the `struct tm` typedef — the FSP RTC API
// uses it (rtc_time_t is a typedef for struct tm). We never call mktime,
// gmtime, tzset, strftime, etc., which hang on bare-metal; see the comment
// block above timestamp_to_utc().
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// IOPORT is opened by RASC's hal_warmstart.c before main() runs — we just
// reach into the generated g_ioport_ctrl for PinWrite calls. No R_IOPORT_Open
// here.

// Nano R4 user LED — yellow, on port 2 pin 4 (P204), configured in RASC.
#define LED_PIN  BSP_IO_PORT_02_PIN_04

void print_RTC();
void delta_timer_cb(timer_callback_args_t * p_args);

// GPT2 (delta_timer) is 16-bit — clamp alarm periods to 0xFFFF ticks.
// At PCLKD/1024 = 31.25 kHz, that's ~2.097 s max one-shot; longer alarms
// are re-armed by the scheduler.
#define DELTA_MAX_TICKS  0xFFFFu

// ── UTC time-of-build helper ───────────────────────────────────────────────
//
// DO NOT call newlib's mktime(), tzset(), or sscanf() here. On bare-metal
// newlib:
//   · mktime()/tzset() pull in lock retargeting and sometimes _sbrk /
//     _gettimeofday. If those aren't fully satisfied at link time the call
//     spins forever.
//   · sscanf() drags in the full stdio FILE machinery, locale init, and in
//     some builds _malloc_r / _sbrk_r. If the heap isn't wired up the
//     malloc retry path can also spin forever.
// Observed symptom of both: init_clocks() never returns, firmware hangs
// silently between boot_alive_blink and usart_transport_init — no output,
// no heartbeat, no hint that the hang is inside a time-conversion call.
//
// __TIMESTAMP__ has a rigid fixed-width format — "Www Mmm DD HH:MM:SS YYYY"
// — so a fixed-offset positional parser does the job with zero libc.
static Long timestamp_to_utc(const char *ts) {
    static const char mon_names[12][3] = {
        {'J','a','n'},{'F','e','b'},{'M','a','r'},{'A','p','r'},
        {'M','a','y'},{'J','u','n'},{'J','u','l'},{'A','u','g'},
        {'S','e','p'},{'O','c','t'},{'N','o','v'},{'D','e','c'}
    };
    // Tiny positional decimal reader: skips any non-digit (covers the
    // space-padded single-digit day that some compilers emit).
    #define DIG(c) ((c) >= '0' && (c) <= '9' ? (c) - '0' : 0)

    // Offsets:  "Www Mmm DD HH:MM:SS YYYY"
    //            0   4   8  11 14 17 20
    int day   = DIG(ts[8])  * 10 + DIG(ts[9]);
    int hour  = DIG(ts[11]) * 10 + DIG(ts[12]);
    int min   = DIG(ts[14]) * 10 + DIG(ts[15]);
    int sec   = DIG(ts[17]) * 10 + DIG(ts[18]);
    int year  = DIG(ts[20]) * 1000 + DIG(ts[21]) * 100
              + DIG(ts[22]) * 10   + DIG(ts[23]);
    #undef DIG

    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (ts[4] == mon_names[i][0] &&
            ts[5] == mon_names[i][1] &&
            ts[6] == mon_names[i][2]) {
            month = i + 1;
            break;
        }
    }

    // Howard Hinnant's days_from_civil — returns days since 1970-01-01,
    // correct for the full proleptic Gregorian range, no libc calls.
    int y   = (month <= 2) ? (year - 1) : year;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                          // [0, 399]
    unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u
                 + (unsigned)(day - 1);                                // [0, 365]
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;           // [0, 146096]
    long days = (long)era * 146097L + (long)doe - 719468L;             // epoch 1970-01-01

    return (Long)(days * 86400L + (long)hour * 3600L + (long)min * 60L + (long)sec);
}

// ── Hardware RTC (IRTC on LOCO — internal ~32 kHz RC oscillator) ──────────
//
// The RA4M1 IRTC runs off LOCO on this board. The Nano R4 schematic does
// NOT populate a 32.768 kHz sub-clock crystal — only the 16 MHz main XO —
// so SOSC is dead and LOCO is the only viable count source. LOCO has
// ±15% accuracy per datasheet, which is poor for wall time, but it keeps
// counting across warm resets and the RTC is self-correcting once a
// trusted time source (NTP, GPS, CLI set-utc) is applied.
//
// Configuration lives in ra_gen/hal_data.c (.clock_source = LOCO), and
// bsp_cfg.h is patched to BSP_CLOCK_CFG_SUBCLOCK_POPULATED=0 so BSP does
// not try to start SOSC (which would hang or burn a full second waiting
// for an oscillator that can never come up).
//
// FSP's rtc_time_t is typedef'd to struct tm. Standard C semantics apply:
// tm_year = years since 1900, tm_mon = 0..11.
//
// set_utc(utc):           epoch → struct tm → R_RTC_CalendarTimeSet
// tick_get_utc():         R_RTC_CalendarTimeGet → struct tm → epoch
//
// On boot the RTC is seeded from __TIMESTAMP__, but only if the hardware
// holds a time earlier than the build timestamp (or obvious garbage). That
// preserves RTC progress across warm resets while still initializing a
// cold-boot RTC that holds nonsense.

// Epoch → struct tm using Hinnant's civil_from_days. No libc time calls.
static void epoch_to_tm(Long utc, struct tm *t) {
    long days = (long)utc / 86400L;
    long sod  = (long)utc - days * 86400L;
    if (sod < 0) { sod += 86400L; days -= 1L; }

    t->tm_hour = (int)(sod / 3600L);
    t->tm_min  = (int)((sod % 3600L) / 60L);
    t->tm_sec  = (int)(sod % 60L);

    // Day of week: 1970-01-01 was Thursday → wday 4 at days 0.
    long wd = ((days % 7L) + 4L) % 7L;
    if (wd < 0) wd += 7L;
    t->tm_wday = (int)wd;

    // civil_from_days (epoch shifted to 0000-03-01 internally).
    days += 719468L;
    long era = (days >= 0 ? days : days - 146096L) / 146097L;
    unsigned doe = (unsigned)(days - era * 146097L);                    // [0, 146096]
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    long y       = (long)yoe + era * 400L;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp  = (5u * doy + 2u) / 153u;                              // [0, 11]
    unsigned d   = doy - (153u * mp + 2u) / 5u + 1u;                    // [1, 31]
    unsigned m   = mp < 10u ? mp + 3u : mp - 9u;                        // [1, 12]
    int year     = (int)(y + (m <= 2 ? 1 : 0));

    t->tm_year  = year - 1900;
    t->tm_mon   = (int)m - 1;
    t->tm_mday  = (int)d;
    t->tm_yday  = 0;                     // FSP doesn't use; 0 is fine
    t->tm_isdst = 0;
}

// struct tm → epoch using Hinnant's days_from_civil. No libc time calls.
static Long tm_to_epoch(const struct tm *t) {
    int year  = t->tm_year + 1900;
    int month = t->tm_mon + 1;
    int day   = t->tm_mday;
    int hour  = t->tm_hour;
    int min   = t->tm_min;
    int sec   = t->tm_sec;

    int y   = (month <= 2) ? (year - 1) : year;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u
                 + (unsigned)(day - 1);
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days    = (long)era * 146097L + (long)doe - 719468L;

    return (Long)(days * 86400L + (long)hour * 3600L + (long)min * 60L + (long)sec);
}

void set_utc(Long utc) {
    struct tm t;
    epoch_to_tm(utc, &t);
    R_RTC_CalendarTimeSet(&g_rtc0_ctrl, &t);
}

Long tick_get_utc(void) {
    struct tm t;
    if (R_RTC_CalendarTimeGet(&g_rtc0_ctrl, &t) != FSP_SUCCESS) {
        return 0;
    }
    return tm_to_epoch(&t);
}

void over_due() { /* incCtr(overDueTea); */ }

// ── delta alarm (one-shot GPT2) ─────────────────────────────────────────────
//
// Translate a delta (in ticks) into a one-shot GPT2 period and fire. GPT2 is
// 16-bit, so periods > 0xFFFF are clamped; the TEA scheduler re-arms when
// this shorter alarm fires.
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

// Called from delta_timer_cb (FSP callback, below). Signals the scheduler.
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

// ── uptime in 32 µs ticks ───────────────────────────────────────────────────
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
    print("  UTC:");
    printDec(get_utc());
    print("  ");
    print_RTC();
}

// WFI is portable across Cortex-M — sleeps until any interrupt wakes the core.
void micro_sleep() { __WFI(); }

// ── LED heartbeat ──────────────────────────────────────────────────────────
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
//
// RTC_BRINGUP_STAGE lets us bisect the hang in the RTC init path without
// losing the CLI. Each stage is strictly additive: if stage N boots to a
// live heartbeat+CLI but stage N+1 hangs, the offending call is the one
// added between them.
//
//   0 — no RTC init at all (last known-good if RTC is the culprit)
//   1 — un-MSTP the RTC only
//   2 — add R_RTC_Open()
//   3 — add R_RTC_CalendarTimeGet()
//   4 — add R_RTC_CalendarTimeSet() (full bring-up)
//
// Test one step at a time, then ratchet back to 4 once all steps pass.
#ifndef RTC_BRINGUP_STAGE
#define RTC_BRINGUP_STAGE 4
#endif

void init_clocks() {
    // IOPORT was opened by R_BSP_WarmStart() during BSP bring-up, before main().
    // DWT cycle counter was enabled in hal_entry.c.
    never(alarmEvent);

    // ── GPT bring-up (RAM-copy cfg override) ──────────────────────────────
    //
    // RASC's configurator picks the PCLKD prescaler (source_div) automatically
    // from "Period × Unit vs counter width", and there's no UI knob to pin it
    // down. For tick_timer we specifically need /1024 (so 32 µs/tick aligns
    // with ONE_SECOND=31250 in project_defs.h) — but RASC's heuristic will
    // happily pick /4 depending on the Period value set. Rather than play
    // ping-pong with that heuristic (and risk silent timing breakage after
    // every regen), we take a RAM-resident copy of each cfg and patch the
    // fields we care about before R_GPT_Open. `static` is mandatory —
    // R_GPT_Open stores a pointer into the cfg for the lifetime of the timer,
    // so it must outlive init_clocks().
    //
    // This removes the last two hand-edits from ra_gen/hal_data.c: RASC can
    // now regenerate freely and nothing on this side needs restoring.

    // tick_timer: 32-bit free-running counter in 32 µs units (PCLKD/1024).
    // Rollover ≈ 19 h (0x7FFFFFFF ticks). Read by get_ticks() for the TEA
    // scheduler; never re-programmed at runtime, so period_counts here is
    // the effective rollover point.
    static timer_cfg_t tick_cfg;
    tick_cfg = tick_timer_cfg;
    tick_cfg.source_div        = (timer_source_div_t)10;   /* /1024 */
    tick_cfg.period_counts     = 0x7FFFFFFFu;
    tick_cfg.duty_cycle_counts = 0x3FFFFFFFu;
    R_GPT_Open  (&tick_timer_ctrl, &tick_cfg);
    R_GPT_Start (&tick_timer_ctrl);

    // delta_timer: 16-bit one-shot, started on demand by set_delta_alarm().
    // period_counts here is only the *initial* value — set_delta_alarm calls
    // R_GPT_PeriodSet every time it fires and overrides GTPR. But the
    // prescaler is latched at Open time, so source_div=10 is the important
    // field.
    static timer_cfg_t delta_cfg;
    delta_cfg = delta_timer_cfg;
    delta_cfg.source_div        = (timer_source_div_t)10;
    delta_cfg.period_counts     = 0xFFFFu;
    delta_cfg.duty_cycle_counts = 0x7FFFu;
    R_GPT_Open  (&delta_timer_ctrl, &delta_cfg);

    later(blink_leds);
    namedAction(blink_leds);

    // ── Hardware RTC bring-up (staged — see RTC_BRINGUP_STAGE above) ───────
    // LOCO is always running after reset (no external crystal required),
    // so no BSP-side stabilization wait is needed.
    //
    // *** FSP QUIRK (verified against r_rtc.c in FSP 6.2.0): ***
    // Unlike every other FSP driver (r_gpt, r_sci_uart, r_elc, r_dtc, …) the
    // RTC driver's R_RTC_Open() does NOT call R_BSP_MODULE_START(FSP_IP_RTC,
    // 0). On RA4M1 the RTC's MSTPCRD[23] bit is 1 after reset (module
    // stopped), so writes to R_RTC->RCRn are silently dropped and the
    // FSP_HARDWARE_REGISTER_WAIT calls inside r_rtc_set_clock_source() spin
    // forever. Un-MSTP the RTC ourselves first.
#if RTC_BRINGUP_STAGE >= 1
    R_BSP_MODULE_START(FSP_IP_RTC, 0);
#endif

#if RTC_BRINGUP_STAGE >= 2
    (void)R_RTC_Open(&g_rtc0_ctrl, &g_rtc0_cfg);
#endif

#if RTC_BRINGUP_STAGE >= 3
    // Seeding policy: if the RTC already holds a sane, recent time, keep it.
    // Otherwise fall through to stage 4 and seed from __TIMESTAMP__.
    Long build_utc = timestamp_to_utc(__TIMESTAMP__);
    bool seed = true;
    struct tm rtc_now;
    if (R_RTC_CalendarTimeGet(&g_rtc0_ctrl, &rtc_now) == FSP_SUCCESS) {
        // Year 2020 threshold rejects an uninitialised/garbage RTC.
        if (rtc_now.tm_year >= 120 && tm_to_epoch(&rtc_now) >= build_utc) {
            seed = false;
        }
    }
#endif

#if RTC_BRINGUP_STAGE >= 4
    if (seed) {
        set_utc(build_utc);
    }
#endif

    // NOTE: do not print() here — init_clocks runs before
    // usart_transport_init(), so emitq has no drain path and output() will
    // spin forever if we enqueue anything. The build banner is printed from
    // hal_entry() after the transport is up.
}

// Print the build timestamp banner. Safe to call ONLY after
// usart_transport_init() has wired up the output() drain.
void print_build_banner(void) {
    print("\nBuilt: "), print(__TIMESTAMP__);
}
