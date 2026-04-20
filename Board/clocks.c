// clocks.c — Nano R4 (RA4M1) port
//
// Time base for TimbreOS on Renesas RA4M1 using FSP GPT peripherals.
//
// CLOCK ARCHITECTURE (post-cascade-failure):
//   The original design cascaded GPT3 (prescalar) → GPT0/GPT2 via
//   GPT_SOURCE_GPT_A. That ELC route does NOT carry GPT3's overflow on
//   RA4M1 — the GPT_SOURCE_GPT_x letters are fixed hardware slots, not
//   a "pick any GPT channel by letter" knob. Both downstream timers
//   stayed at 0 forever. Now both run directly off PCLKD with /1024.
//
//   tick_timer  — GPT0,  32-bit free-running uptime counter.
//                 PCLKD/1024 @ 32 MHz = 31.25 kHz → 32 µs/tick.
//                 Period = 0x7fffffff (rollover ≈ 38 hours).
//   delta_timer — GPT2,  16-bit one-shot alarm used by set_delta_alarm().
//                 PCLKD/1024 @ 32 MHz, same 32 µs tick units → 16-bit
//                 max one-shot ≈ 2.097 s; longer alarms re-arm.
//   prescalar   — GPT3,  legacy / unused; left configured but not opened.
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
static void init_clocks_diag_pulse2(void);

// prescalar is no longer in the chain — left behind for diagnostics only.
// #define PRESCALAR_PERIOD_COUNTS  (CLOCK_MHZ * 1000000u / ONE_SECOND)

// GPT2 (delta_timer) is 16-bit — clamp alarm periods to 0xFFFF ticks.
// At PCLKD/1024 = 31.25 kHz, that's ~2.097 s max one-shot; longer alarms
// are re-armed by the scheduler.
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
    static int first_call = 1;

    if (t > DELTA_MAX_TICKS) {
        print("#");
        t = DELTA_MAX_TICKS;
    }
    if (t < 1) t = 1;
    R_GPT_PeriodSet(&delta_timer_ctrl, (uint32_t)t);
    R_GPT_Reset    (&delta_timer_ctrl);
    R_GPT_Start    (&delta_timer_ctrl);   // one-shot — stops itself at TRG

    // BISECT (FIRST CALL ONLY): does GPT2's counter actually advance?
    // Sample status.counter twice with a busy-loop between. If it advanced,
    // the counter is running — 2 quick pulses. If stuck at 0, flash 1 long.
    if (first_call) {
        first_call = 0;
        timer_status_t s1, s2;
        R_GPT_StatusGet(&delta_timer_ctrl, &s1);
        for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        R_GPT_StatusGet(&delta_timer_ctrl, &s2);
        if (s2.counter > s1.counter) {
            init_clocks_diag_pulse2();   // 2 quick: GPT2 IS counting
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // BISECT: where in the NVIC/VTOR chain are we failing?
        //   (a) Is NVIC ISER bit 5 set after R_GPT_Open? 2 quick = yes, 1 long = no.
        //   (b) Is SCB->VTOR pointing at 0x4000 (our relocated vectors)?
        //       2 quick = yes (== 0x4000), 1 long = no (wrong VTOR).
        //   (c) Force-enable NVIC ISER bit 5, then manually pend IRQ 5.
        //       If the ISR blip (single short pulse from delta_timer_cb) fires,
        //       the NVIC+vector-table path works. If not, it's a vector-content
        //       problem at offset 0x40+4*5 = 0x54 from VTOR.
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }

        // (a) NVIC ISER bit 5
        if (NVIC->ISER[0] & (1u << 5)) {
            init_clocks_diag_pulse2();   // 2 quick
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // (b) VTOR == 0x4000 ?
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
        if (SCB->VTOR == 0x00004000u) {
            init_clocks_diag_pulse2();   // 2 quick
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // (c) Does the vector at VTOR+0x54 actually point at gpt_counter_overflow_isr?
        //     IRQ 5 vector lives at VTOR + 0x40 + 5*4 = VTOR + 0x54.
        //     Cortex-M function pointers have the thumb bit set, so compare with |1.
        extern void gpt_counter_overflow_isr(void);
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
        uint32_t *vt       = (uint32_t *)SCB->VTOR;
        uint32_t  vec_irq5 = vt[16 + 5];  // 16 system vectors, then IRQ 0, 1, ..., 5
        uint32_t  want     = ((uint32_t)&gpt_counter_overflow_isr) | 1u;
        if (vec_irq5 == want) {
            init_clocks_diag_pulse2();   // 2 quick — vector content correct
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // (d) Is PRIMASK set (IRQs globally disabled)?
        //     2 quick = enabled, 1 long = masked.
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
        if (__get_PRIMASK() == 0) {
            init_clocks_diag_pulse2();   // 2 quick — PRIMASK clear
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // (e) Are we in an ISR context (IPSR != 0)?
        //     2 quick = thread mode (good), 1 long = ISR mode (bad).
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
        if (__get_IPSR() == 0) {
            init_clocks_diag_pulse2();   // 2 quick — thread mode
        } else {
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }

        // (f) Force-enable IRQs globally, enable NVIC ISER bit 5, pend.
        //     If everything above says good, this MUST fire the ISR blip.
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
        __enable_irq();
        NVIC_SetPriority((IRQn_Type)5, 12);
        NVIC_EnableIRQ((IRQn_Type)5);
        NVIC_SetPendingIRQ((IRQn_Type)5);
        for (volatile uint32_t w = 0; w < 1500000; w++) { __asm volatile ("nop"); }
    }
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
// BISECT: temporarily disabled. If the scheduler starts running after this
// change, the GPT2 IRQ isn't firing (check RASC: delta_timer priority set?).
void micro_sleep() { /* __WFI(); */ }

// ── LED blink ──────────────────────────────────────────────────────────────
// Nano R4 yellow LED on P204. Classic two-flash heartbeat — short-on, short-gap,
// short-on, long-gap.
#define ON_TIME 2
static inline void led_on (void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH); }
static inline void led_off(void) { R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);  }

static void blink_leds() {
    // BISECT: one pulse burst the first time the scheduler dispatches us.
    // If we see this after the "5 quick" group, dispatch works and the
    // heartbeat logic/IRQ chain is the issue. If silent, scheduler is stuck.
    static int dispatched = 0;
    if (dispatched++ == 0) { init_clocks_diag_pulse2(); }

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

// Bring-up diagnostic — 2 quick LED pulses after all three GPTs are open.
// Matches the pattern set up in src/hal_entry.c. Uses a volatile nop loop
// because the tick_timer isn't guaranteed to be running when called.
static void init_clocks_diag_pulse2(void)
{
    for (volatile uint32_t w = 0; w < 2000000; w++) { __asm volatile ("nop"); }
    for (int i = 0; i < 2; i++) {
        R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_02_PIN_04, BSP_IO_LEVEL_HIGH);
        for (volatile uint32_t w = 0; w < 150000; w++) { __asm volatile ("nop"); }
        R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_02_PIN_04, BSP_IO_LEVEL_LOW);
        for (volatile uint32_t w = 0; w < 250000; w++) { __asm volatile ("nop"); }
    }
}

// ── init ───────────────────────────────────────────────────────────────────
void init_clocks() {
    // DWT cycle counter was enabled in hal_entry.c; IOPORT was opened by
    // R_BSP_WarmStart() during BSP bring-up, before main().

    never(alarmEvent);

    // prescalar left unopened — no longer feeds anything (see header comment).
    // R_GPT_Open(&prescalar_ctrl, &prescalar_cfg); R_GPT_Start(&prescalar_ctrl);

    // tick_timer: 32-bit free-running counter in 32 µs units (PCLKD/1024).
    R_GPT_Open  (&tick_timer_ctrl, &tick_timer_cfg);
    R_GPT_Start (&tick_timer_ctrl);

    // delta_timer: 16-bit one-shot, started on demand by set_delta_alarm().
    R_GPT_Open  (&delta_timer_ctrl, &delta_timer_cfg);

    init_clocks_diag_pulse2();   // 2 quick: all three GPTs opened

    // BISECT: is tick_timer actually counting? Sample the counter twice,
    // with a fat nop-loop between. If t2 > t1, the GPT3→GPT0 cascade works
    // and we flash 2 quick. If t2 == t1 (counter stuck at zero), the
    // GPT_SOURCE_GPT_A cascade is broken — flash 1 long.
    {
        Long t1 = get_ticks();
        for (volatile uint32_t w = 0; w < 2000000; w++) { __asm volatile ("nop"); }
        Long t2 = get_ticks();
        if (t2 > t1) {
            init_clocks_diag_pulse2();   // 2 quick: cascade OK
        } else {
            // One long "sad" pulse — cascade dead.
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_HIGH);
            for (volatile uint32_t w = 0; w < 2500000; w++) { __asm volatile ("nop"); }
            R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, BSP_IO_LEVEL_LOW);
            for (volatile uint32_t w = 0; w < 500000; w++) { __asm volatile ("nop"); }
        }
    }

    later(blink_leds);
    init_clocks_diag_pulse2();   // 2 quick (#2): later() returned

    namedAction(blink_leds);
    init_clocks_diag_pulse2();   // 2 quick (#3): namedAction returned

    // BISECT: print() is the top suspect for the hang. mocks.c::output()
    // spin-loops on get_ticks() if emitq fills before UART transport is
    // up. Skip for now; re-enable once the heartbeat is confirmed and
    // usart_transport_init is wired up to drain emitq.
    //
    // print("\nBuilt: "), print(__TIMESTAMP__);
    // set_utc(timestamp_to_utc(__TIMESTAMP__));
}
