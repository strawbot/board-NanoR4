// board_cli.c — Nano R4 (RA4M1) CLI command backends.
//
// MVP port: STM32 LL_RCC/LL_TIM/RCC_APBxENR machinery stripped out; show_sys
// and show_timers emit human-readable placeholders so the CLI commands stay
// wired up. Real FSP-based clock/timer surveys will land once the r_cgc stack
// is opened and the GPT+AGT peripherals have accessors on this MCU.

#include <stdbool.h>
#include <stdint.h>

#include "r_dac.h"          // dac_instance_ctrl_t, dac_cfg_t, dac_extended_cfg_t
#include "hal_data.h"
#include "tea.h"
#include "cli.h"
#include "printers.h"
#include "clocks.h"
#include "project_defs.h"
#include "clocks.h"
#include "board_cli.h"

// Stack overflow detection is provided by Robot/diagnostics/canary.
// See Robot/README.md for the linker-symbol contract.
#include "canary.h"

// AD5593R ADC/DAC expander driver lives in Robot/io/ad5593r (board-agnostic
// protocol; see Board/i2c0.c for the IIC0 peripheral it rides on).
#include "ad5593r.h"

// ── clocks & uptime ────────────────────────────────────────────────────────
void show_sys(void) {
    // Clock tree is pinned down in ra_gen/bsp_clock_cfg.h.
    // XTAL 16 MHz → PLL ×8 /2 = 64 MHz → ICLK /2 = 32 MHz; PCLKD /2 = 32 MHz.
    print("SYSCLK:  "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("ICLK:    "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("PCLKD:   "); printDec(CLOCK_MHZ);   print(" MHz"); printCr();
    print("uptime:  "); printDec(get_ticks()); print(" ticks (100 us)"); printCr();
    show_timer();

    print("\nstack:   ");
    stack_render(stack_check());
    printCr();
}

// ── timer survey ───────────────────────────────────────────────────────────
// Shows live GTCNT (and duty for PWM) from each GPT channel's registers.
// R_GPT0/2/3 are register-map pointers from the CMSIS device header;
// GTCNT is the counter, GTPR is the period, GTCCR[1] is GTCCRB (duty).
void show_timers(void) {
    print("timers:"); printCr();

    // GPT0 — tick_timer, free-running 32-bit up-counter (PCLKD/4 = 8 MHz)
    print("  GPT0 tick     free-run  cnt=");
    printDec(R_GPT0->GTCNT);
    printCr();

    // GPT2 — delta_timer, one-shot alarm for TEA scheduler (PCLKD/1024 = 31.25 kHz)
    print("  GPT2 delta    one-shot  cnt=");
    printDec(R_GPT2->GTCNT);
    print("  pr=");
    printDec(R_GPT2->GTPR);
    printCr();

    // GPT3 — muscle-wire PWM, 10 kHz saw-wave (PCLKD/8 = 4 MHz, period=400)
    print("  GPT3 mw-pwm   10kHz     cnt=");
    printDec(R_GPT3->GTCNT);
    print("  duty=");
    printDec(R_GPT3->GTCCR[1]);   // GTCCRB controls GTIOC3B output
    print("/");
    printDec(R_GPT3->GTPR);
    printCr();

    // RTC — hardware IRTC on 32.768 kHz sub-clock
    print("  RTC  irtc     32kHz     ");
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
// get_utc() is backed by the hardware IRTC running off the 32.768 kHz
// sub-clock XTAL; Board/clocks.c seeds it from __TIMESTAMP__ at boot if the
// RTC doesn't already hold a later valid time (preserves progress across
// warm resets). The formatting below is epoch-agnostic — works for either
// software or hardware time sources.
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

// ── CLI wrapper: set RTC from a Unix epoch on the data stack ──────────────
// Usage: `<epoch> set-utc`  — e.g. `1745000000 set-utc`
// The bound CLI contract is `void f(void)`, so we pop the argument off the
// TEA data stack with ret() and hand it to the typed setter in clocks.c.
void cli_set_utc(void) {
    Long utc = (Long)ret();
    set_utc(utc);
    print("set: "); print_RTC(); printCr();
}

// ── gpio_dump_all stub ─────────────────────────────────────────────────────
// gpio_dump.c is still STM32-only and excluded from the build. Provide a
// placeholder so the CLI word `pins` resolves.
void gpio_dump_all(void) {
    print("pins: gpio_dump not ported yet"); printCr();
}

// ── DAC output on P014 ────────────────────────────────────────────────────
// Usage: `<value> dac!`  — writes a 12-bit value (0–4095) to DAC0/P014.
//
// DAC instance is defined locally (not generated by RASC/hal_data).
// Channel 0 = P014; 12-bit right-aligned; no charge pump; no amplifier.

static dac_instance_ctrl_t g_dac0_ctrl;  // DO NOT pre-initialize (FSP requirement)

static const dac_extended_cfg_t g_dac0_cfg_extend = {
    .enable_charge_pump      = false,
    .output_amplifier_enabled = false,
    .internal_output_enabled  = false,
    .data_format              = DAC_DATA_FORMAT_FLUSH_RIGHT,
};

static const dac_cfg_t g_dac0_cfg = {
    .channel           = 0U,    // DAC0 = P014
    .ad_da_synchronized = false,
    .p_extend          = &g_dac0_cfg_extend,
};

void dac_init(void) {
    R_DAC_Open(&g_dac0_ctrl, &g_dac0_cfg);
    R_DAC_Write(&g_dac0_ctrl, 0);
    R_DAC_Start(&g_dac0_ctrl);
}

void dac_set(void) {
    Cell v = ret();
    if (v < 0)    v = 0;
    if (v > 4095) v = 4095;
    R_DAC_Write(&g_dac0_ctrl, (uint16_t)v);
    // print("dac: "); printDec(v); printCr();
}

Short dac_get () {
    dac_ctrl_t * p_api_ctrl = &g_dac0_ctrl;
    dac_instance_ctrl_t * p_ctrl = (dac_instance_ctrl_t *) p_api_ctrl;

    /* Read the value from the D/A converter. */
    return p_ctrl->p_reg->DADR[p_ctrl->channel_index];
}

void ramp_up() {
    Cell v = 0;
    do {
        R_DAC_Write(&g_dac0_ctrl, v);
        v = (v + 1) & 0xFFF;
    } while(v);
}

void ramp_down() {
    Cell v = 0xFFF;
    do {
        R_DAC_Write(&g_dac0_ctrl, v);
        v = (v - 1) & 0xFFF;
    } while (v != 0xFFF);
}

// ── AD5593R (IIC0: SCL=P400, SDA=P401) ──────────────────────────────────────
// Chip-level protocol lives in Robot/io/ad5593r.c; these are just the CLI
// stack-word wrappers. Channel numbering: dac 0-3 = I/O0-I/O3, adc 0-3 =
// I/O4-I/O7 (see ad5593r.h).

void ad5593r_show_status(void) {
    ad5593r_status_t st;
    if (!ad5593r_read_status(&st)) {
        print("ad5593r: read failed"); printCr();
        return;
    }
    print("ad5593r: dac_cfg=0x"); printHex(st.dac_config);
    print(" adc_cfg=0x");         printHex(st.adc_config);
    print(" ref=");               print(st.ref_enabled ? "on" : "off");
    printCr();
}

void ad5593r_cli_dac_set(void) { // ( v ch -- )
    Cell ch = ret();
    Cell v  = ret();
    if (v < 0)    v = 0;
    if (v > 4095) v = 4095;
    if (!ad5593r_set_dac((uint8_t)ch, (uint16_t)v)) {
        print("ad5593r: dac write failed"); printCr();
    }
}

void ad5593r_cli_adc_read(void) { // ( ch -- mv )
    Cell ch = ret();
    uint16_t code;
    if (!ad5593r_read_adc((uint8_t)ch, &code)) {
        print("ad5593r: adc read failed"); printCr();
        lit(-1);
        return;
    }
    // Internal 2.5V reference, 0-VREF range (see ad5593r_init()).
    lit((Cell)((uint32_t)code * 2500U / 4095U));
}

// ── word filter ───────────────────────────────────────────────────────────
bool visible_word(char *s) { (void)s; return true; }
