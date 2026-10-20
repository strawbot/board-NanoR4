// Board/analog_test.c — see analog_test.h.
//
// GPT1 free-runs and overflows at a fixed rate; that overflow is wired
// straight into a DMAC channel's activation_source (no CPU interrupt, no
// ELC_Open/LinkSet call — dmac_extended_cfg_t::activation_source does the
// link internally), so the ramp runs with zero CPU involvement once armed.
//
// This was originally GPT0, on the (unverified) assumption that it was
// free. It is not: Board/clocks.c's tick_timer uses GPT0 as TEA's own
// system tick (get_ticks()), GPT2 is delta_timer, and GPT7 is the muscle
// frame timer (frame_timer.c). Reconfiguring GPT0 here was stomping the
// scheduler's own timebase — on the bench this showed up as the time-event
// list freezing and the status LED's heartbeat blink sticking, both
// symptoms of TEA's tick no longer advancing, not of anything specific to
// DAC12/DAC8/ACMPLP. GPT1 is not claimed anywhere else in this project.
//
// Register-level details (COMPSEL0/1, the ACMPLP setup procedure, DACR's
// actual bit layout, P110's VCOUT pin function) are confirmed against the
// RA4M1 Hardware User's Manual (Docs/r01uh0887ej0110-ra4m1.pdf) at their
// point of use below, not just assumed from the CMSIS header.

#include "hal_data.h"
#include "r_gpt.h"
#include "r_dmac.h"
#include "r_ioport.h"
#include "analog_test.h"
#include "board_cli.h"
#include "cli.h"
#include "printers.h"
#include "tea.h"

static ioport_instance_ctrl_t g_at_ioport_ctrl;
static void analog_test_dac8_cmp_continue(void);
static void analog_test_opamp0_continue(void);
static void analog_test_opamp0_sweep_step(void);
static void analog_test_opamp0_sweep_read(void);

// GPT1 tick: PCLKD (48 MHz, per frame_timer.c) / 16 = 3 MHz, the same
// divisor frame_timer.c already uses successfully for GPT7. An earlier
// version of this file used TIMER_SOURCE_DIV_32, which measured out at
// exactly 2x the intended rate (489 Hz instead of 244 Hz on the bench).
// The manual's GTCR.TPCS[2:0] table only defines six real GPT divider
// ratios — /1, /4, /16, /64, /256, /1024 — there is no /32 on this
// hardware at all. r_gpt.c's own FSP_ASSERT requires source_div to be
// even for exactly this reason (the shared timer_source_div_t enum also
// serves peripherals that do support the odd/power-of-two-only values GPT
// doesn't); that assert is compiled out in a release build, so the
// invalid value silently bit-shifted onto the same TPCS code as /16
// instead of being rejected. TIMER_SOURCE_DIV_16 below is the real thing.
#define GPT1_HZ      3000000U
#define RAMP_STEPS   64U
#define RAMP_HZ      15625U                              // 3 MHz / 192
#define GPT1_PERIOD  (GPT1_HZ / RAMP_HZ)                  // counts per DAC step

static gpt_instance_ctrl_t   g_at_gpt_ctrl;
static dmac_instance_ctrl_t  g_at_dmac_ctrl;
// BSP_CFG_PARAM_CHECKING_ENABLE is 0 in this project (ra_cfg/fsp_cfg/bsp/
// bsp_cfg.h), which compiles out FSP's own "was this opened?" guards in
// R_GPT_Stop/Close and R_DMAC_Disable/Close — calling any of them on a
// control block that was never opened dereferences a NULL p_reg/p_cfg and
// hard-faults. Track open state ourselves so analog_test_stop_timers() can
// be called unconditionally (as opamp0-sweep does, first thing, even on a
// cold boot where neither was ever opened) without that risk.
static bool g_at_gpt_open;
static bool g_at_dmac_open;

// Triangle lookup table, 12-bit full-scale (DAC12) shifted down to 8-bit
// (DAC8) by the caller as needed — see the two start functions below.
static uint16_t g_ramp12[RAMP_STEPS];
static uint8_t  g_ramp8[RAMP_STEPS];

// Was: ascending/descending halves built by mirroring index i to
// RAMP_STEPS-1-i, which (for an even RAMP_STEPS split into two equal
// halves) wrote the peak value to two adjacent samples — a genuine
// 2-sample-wide flat top, not a scope/measurement artifact. This version
// gives a single-sample peak (at k=RAMP_STEPS/2) and single-sample valley
// (at k=0): min(k, RAMP_STEPS-k) counts 0..32..0 with no repeated value.
static void build_ramp(void) {
    for (uint32_t k = 0; k < RAMP_STEPS; k++) {
        uint32_t tri = (k < RAMP_STEPS / 2U) ? k : (RAMP_STEPS - k);
        uint32_t v12 = (4095U * tri) / (RAMP_STEPS / 2U);
        g_ramp12[k] = (uint16_t) v12;
        g_ramp8[k]  = (uint8_t) (v12 >> 4);
    }
}

static const gpt_extended_cfg_t g_at_gpt_ext = {
    .gtioca = { .output_enabled = false, .stop_level = GPT_PIN_LEVEL_LOW },
    .gtiocb = { .output_enabled = false, .stop_level = GPT_PIN_LEVEL_LOW },
    .start_source      = (gpt_source_t) GPT_SOURCE_NONE,
    .stop_source       = (gpt_source_t) GPT_SOURCE_NONE,
    .clear_source      = (gpt_source_t) GPT_SOURCE_NONE,
    .count_up_source   = (gpt_source_t) GPT_SOURCE_NONE,
    .count_down_source = (gpt_source_t) GPT_SOURCE_NONE,
    .capture_a_source  = (gpt_source_t) GPT_SOURCE_NONE,
    .capture_b_source  = (gpt_source_t) GPT_SOURCE_NONE,
    .capture_filter_gtioca = GPT_CAPTURE_FILTER_NONE,
    .capture_filter_gtiocb = GPT_CAPTURE_FILTER_NONE,
    .capture_a_ipl = BSP_IRQ_DISABLED,
    .capture_b_ipl = BSP_IRQ_DISABLED,
    .capture_a_irq = FSP_INVALID_VECTOR,
    .capture_b_irq = FSP_INVALID_VECTOR,
    .compare_match_value  = { 0U, 0U },
    .compare_match_status = 0U,
    .p_pwm_cfg = NULL,
    .gtior_setting.gtior = 0U,
    .gtioca_polarity = GPT_GTIOC_POLARITY_NORMAL,
    .gtiocb_polarity = GPT_GTIOC_POLARITY_NORMAL,
};

// No CPU interrupt: GPT1's overflow event feeds the DMAC's activation_source
// directly in hardware. cycle_end_irq is FSP_INVALID_VECTOR on purpose.
static const timer_cfg_t g_at_gpt_cfg = {
    .mode              = TIMER_MODE_PERIODIC,
    .period_counts     = GPT1_PERIOD,
    .duty_cycle_counts = 0U,
    .source_div        = TIMER_SOURCE_DIV_16,             // 48 MHz / 16 = 3 MHz, matches GPT1_HZ above (same divisor as frame_timer.c's GPT7)
    .channel           = 1U,                              // GPT1 — GPT0/GPT2/GPT7 are claimed (Board/clocks.c, frame_timer.c)
    .p_callback        = NULL,
    .p_context         = NULL,
    .p_extend          = &g_at_gpt_ext,
    .cycle_end_ipl     = BSP_IRQ_DISABLED,
    .cycle_end_irq     = FSP_INVALID_VECTOR,
};

static dmac_extended_cfg_t g_at_dmac_ext = {
    .channel           = 0U,                              // DMAC0 — unused by this project (see cli_transport_usart.c)
    .irq               = FSP_INVALID_VECTOR,
    .ipl               = BSP_IRQ_DISABLED,
    .offset            = 0,
    .src_buffer_size   = 0U,
    .activation_source = ELC_EVENT_GPT1_COUNTER_OVERFLOW,
    .p_callback        = NULL,
    .p_callback_memory = NULL,
    .p_context         = NULL,
};

static transfer_info_t g_at_dmac_info = {
    .transfer_settings_word_b = {
        // Was backwards: dest=INCREMENTED/src=FIXED meant every transfer
        // re-read the same source sample while walking the DESTINATION
        // address off DAC12's/DAC8's register into whatever memory follows
        // it — for up to 65536 repeats x 64 transfers, essentially
        // unbounded corruption. This is what was actually breaking the
        // tick timer, independent of which GPT channel or peripheral was
        // involved. Correct direction: source (the ramp table) advances
        // and wraps every RAMP_STEPS transfers; destination (one fixed
        // hardware register) never moves.
        .dest_addr_mode = TRANSFER_ADDR_MODE_FIXED,
        .repeat_area    = TRANSFER_REPEAT_AREA_SOURCE,
        .irq            = TRANSFER_IRQ_END,
        .chain_mode     = TRANSFER_CHAIN_MODE_DISABLED,
        .src_addr_mode  = TRANSFER_ADDR_MODE_INCREMENTED,
        .size           = TRANSFER_SIZE_2_BYTE,           // overwritten to 1-byte for the DAC8 test
        .mode           = TRANSFER_MODE_REPEAT,           // wraps after RAMP_STEPS transfers — free-running
    },
    .p_src      = NULL,
    .p_dest     = NULL,
    .num_blocks = 0U,
    .length     = RAMP_STEPS,
};

static const transfer_cfg_t g_at_dmac_cfg = {
    .p_info   = &g_at_dmac_info,
    .p_extend = &g_at_dmac_ext,
};

// gpt1-only — ( -- ) isolation test: R_GPT_Open + R_GPT_Start on GPT1,
// nothing else at all — no DAC12, no DMAC, no ACMPLP. If dac-status shows
// the tick_timer (GPT0) broken after just this, the fault is in GPT1's
// own Open/Start sequence itself, not in the DAC12/DMAC interaction.
void analog_test_cli_gpt1_only(void) {
    fsp_err_t err = R_GPT_Open(&g_at_gpt_ctrl, &g_at_gpt_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: GPT open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_gpt_open = true;
    R_GPT_Start(&g_at_gpt_ctrl);
    print("analog-test: GPT1 opened and started, nothing else touched. Check dac-status / the LED now."); printCr();
}

// dmac-only — ( -- ) isolation test: gpt1-only PLUS R_DMAC_Open/Enable,
// actually triggering transfers into DAC12's DADR — but DAC12's own module
// clock is deliberately left stopped and DACR untouched. If this breaks
// the tick, the fault is in GPT1+DMAC actively triggering together (maybe
// bus arbitration), independent of anything DAC12-specific. If it doesn't,
// the fault is narrowed down to the DAC12 module-start/DACR steps.
void analog_test_cli_dmac_only(void) {
    build_ramp();
    g_at_dmac_info.transfer_settings_word_b.size = TRANSFER_SIZE_2_BYTE;
    g_at_dmac_info.p_src  = g_ramp12;
    g_at_dmac_info.p_dest = (void *) &R_DAC->DADR[0];
    g_at_dmac_info.length = RAMP_STEPS;

    fsp_err_t err = R_GPT_Open(&g_at_gpt_ctrl, &g_at_gpt_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: GPT open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_gpt_open = true;
    err = R_DMAC_Open(&g_at_dmac_ctrl, &g_at_dmac_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: DMAC open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_dmac_open = true;
    R_DMAC_Enable(&g_at_dmac_ctrl);
    R_GPT_Start(&g_at_gpt_ctrl);
    print("analog-test: GPT1+DMAC running, DAC12 left unclocked/untouched. Check dac-status / the LED now."); printCr();
}

static void analog_test_stop_timers(void) {
    // Guarded on our own open-tracking, not FSP's: BSP_CFG_PARAM_CHECKING_
    // ENABLE is 0 project-wide, so R_GPT_Stop/Close and R_DMAC_Disable/
    // Close have no "was this opened?" check compiled in at all — calling
    // them on a control block that was never opened (e.g. opamp0-sweep
    // run as the very first command after a cold boot) dereferences a
    // NULL p_reg/p_cfg and hard-faults the MCU before anything can print.
    if (g_at_dmac_open) { R_DMAC_Disable(&g_at_dmac_ctrl); R_DMAC_Close(&g_at_dmac_ctrl); g_at_dmac_open = false; }
    if (g_at_gpt_open)  { R_GPT_Stop(&g_at_gpt_ctrl); R_GPT_Close(&g_at_gpt_ctrl); g_at_gpt_open = false; }
    R_OPAMP->AMPC = 0x00U;   // AMPE0=0, IREFE=0 — safe to call even if opamp0-test was never run
    // Cancel a pending opamp0-sweep step/read if one is in flight — safe to
    // call even when the sweep was never started (stop_te on an action with
    // no pending entry is a no-op).
    stop_te(analog_test_opamp0_sweep_step);
    stop_te(analog_test_opamp0_sweep_read);
}

void analog_test_cli_stop(void) {
    analog_test_stop_timers();
    print("analog-test: stopped"); printCr();
}

// dac12-ramp — ( -- ) scope P014 directly.
void analog_test_cli_dac12_ramp(void) {
    build_ramp();

    g_at_dmac_info.transfer_settings_word_b.size = TRANSFER_SIZE_2_BYTE;
    g_at_dmac_info.p_src  = g_ramp12;
    g_at_dmac_info.p_dest = (void *) &R_DAC->DADR[0];
    g_at_dmac_info.length = RAMP_STEPS;

    // Dropped R_DAC_Open() in favor of direct register access, matching
    // DAC8/ACMPLP below. (Earlier blamed this for breaking GPT — wrong; the
    // actual cause was the swapped DMA src/dest addr_mode fields above,
    // confirmed by dmac-only still breaking the tick with R_DAC_Open never
    // called at all. This may still have cost us something real, though:
    // R_DAC_Open likely also configures P014's pin function, which nothing
    // here otherwise does — see the IOPORT_PinCfg call just below.)
    R_BSP_MODULE_START(FSP_IP_DAC, 0U);
    // DACR (HWM 36.2.2): bit6=DAOE0, bits4:0 reserved-must-be-1, bit5/7
    // reserved-must-be-0. There is no "DAE" bit on this part — the CMSIS
    // header's DACR_b.DAE (bit 5) is wrong for RA4M1; that bit is reserved
    // and writing 1 to it very likely caused the whole byte write to be
    // rejected, which is why DAOE0 read back 0 too. Write the raw byte.
    R_DAC->DACR = 0x4FU;                                  // 0100_1111: DAOE0=1, reserved bits per spec

    // DAVREFCR (HWM 36.2.5) resets to 000b: "No reference voltage
    // selected." Output = (DADR0/4096) x reference voltage — with no
    // reference, there's nothing to scale against regardless of DAOE0 or
    // DADR0, which is consistent with every symptom so far: registers all
    // correct, nothing on the pin. REF=001b selects AVCC0/AVSS0 (the
    // analog supply rails) — simplest option, no discharge sequencing
    // required the way the internal-reference option needs.
    R_DAC->DAVREFCR = 0x01U;

    // P014 itself was never switched out of GPIO mode — DAOE0=1 and a
    // moving DADR[0] (confirmed via dac-status) mean DAC12 is converting
    // correctly, but nothing reaches the physical pin until it's in analog
    // mode. Same HWM 19.5.4 procedure as CMPIN0/P100: PMR=0, ASEL=1.
    R_IOPORT_Open(&g_at_ioport_ctrl, NULL);
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_14,
                     (uint32_t) IOPORT_CFG_ANALOG_ENABLE);

    fsp_err_t err = R_GPT_Open(&g_at_gpt_ctrl, &g_at_gpt_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: GPT open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_gpt_open = true;
    err = R_DMAC_Open(&g_at_dmac_ctrl, &g_at_dmac_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: DMAC open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_dmac_open = true;
    R_DMAC_Enable(&g_at_dmac_ctrl);
    R_GPT_Start(&g_at_gpt_ctrl);

    print("analog-test: DAC12 (P014) ramping at "); printDec0(RAMP_HZ); print(" Hz, "); printDec0(RAMP_STEPS);
    print(" steps. Scope P014. `analog-test-stop` to end."); printCr();
}

// dac8-cmp — ( -- ) put a known test voltage on CMPIN0 (P100) first,
// then watch VCOUT (P110) for a clean transition as DAC8 ramps through it.
void analog_test_cli_dac8_cmp(void) {
    build_ramp();

    g_at_dmac_info.transfer_settings_word_b.size = TRANSFER_SIZE_1_BYTE;
    g_at_dmac_info.p_src  = g_ramp8;
    g_at_dmac_info.p_dest = (void *) &R_DAC8->DACS[0];
    g_at_dmac_info.length = RAMP_STEPS;

    // Setup procedure per RA4M1 HWM 39.3 (Table 39.3), steps 1-4, 7:
    R_BSP_MODULE_START(FSP_IP_DAC8, 0U);                  // step 1 (DAC8 has no MSTP note in 39.3 but needs its own clock)
    R_BSP_MODULE_START(FSP_IP_ACMPLP, 0U);                // step 1: MSTPCRD.MSTPD29 = 0

    R_DAC8->DAM_b.DAMD0 = 0U;                             // normal mode, channel 0
    R_DAC8->DAM_b.DACE0 = 1U;                             // enable DAC8 channel 0
    print("analog-test: DACE0 immediately after write reads "); printDec0(R_DAC8->DAM_b.DACE0); printCr();

    R_IOPORT_Open(&g_at_ioport_ctrl, NULL);
    // step 2: P100 as a pure analog input. Per HWM 19.5.4 ("Notes on Using
    // Analog Functions"), PMR and PDR must be 0 (general input port) with
    // only ASEL set — PSEL/peripheral-mode is NOT part of how analog inputs
    // are routed on this chip, unlike digital peripheral pins.
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_01_PIN_00,
                     (uint32_t) IOPORT_CFG_ANALOG_ENABLE);

    // Confirmed against the manual (COMPSEL0/COMPSEL1, 39.2.4/39.2.5):
    // CRVS[2:0]=010b selects DAC8 ch0 as IVREF0; CMPSEL[2:0]=001b selects
    // CMPIN0(P100) as IVCMP0.
    R_ACMPLP->COMPOCR_b.SPDMD = 0U;                       // step 3: low-speed mode (reset default; explicit for clarity)
    R_ACMPLP->COMPMDR_b.C0WDE = 0U;                       // step 4: window mode disabled
    R_ACMPLP->COMPMDR_b.C0VRF = 0U;                       // step 4: reference = CRVS-selected source, not internal Vref
    R_ACMPLP->COMPSEL1_b.IVREF0 = 0x2U;                   // step 4: DAC8 (channel 0) output
    R_ACMPLP->COMPSEL0_b.IVCMP0 = 0x1U;                   // step 2: CMPIN0 (P100)
    R_ACMPLP->COMPMDR_b.C0ENB = 1U;                       // step 4: enable comparator 0

    // step 5: Tcmp, min 100us, before trusting C0MON/VCOUT. later() (used
    // in an earlier version of this file) runs on the very next scheduler
    // pass with no guaranteed minimum delay at all, so it wasn't actually
    // enforcing Tcmp. after() is the right one: non-blocking, queued via
    // later() once the real delay elapses, not interrupt-level and not
    // landing at an exact time the way in() does — just a genuine minimum
    // wait. msec is the floor, so 1ms (10x the 100us minimum, harmless to
    // overshoot) rather than something finer that doesn't exist.
    after(msec(1), analog_test_dac8_cmp_continue);
}

static void analog_test_dac8_cmp_continue(void) {
    // step 7 (after Tcmp, per Table 39.3): VCOUT pin function, then enable its output.
    // VCOUT is P110, NOT P109 (confirmed against HWM Table 19.7: PSEL=01001b
    // is "CLKOUT/ACMPLP/RTC" on P110 specifically, CLKOUT-only on P109).
    // IOPORT_PERIPHERAL_ACMPHS (0x0C) was the wrong constant for either pin
    // — that value is CTSU on this part. IOPORT_PERIPHERAL_CLKOUT_COMP_RTC
    // (0x09) is the one Table 19.7 actually specifies.
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_01_PIN_10,
                     (uint32_t) (IOPORT_PERIPHERAL_CLKOUT_COMP_RTC | IOPORT_CFG_PERIPHERAL_PIN));
    R_ACMPLP->COMPOCR_b.C0OE = 1U;                        // drive VCOUT from comparator 0
    R_ACMPLP->COMPOCR_b.C0OP = 0U;                        // non-inverted

    fsp_err_t err = R_GPT_Open(&g_at_gpt_ctrl, &g_at_gpt_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: GPT open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_gpt_open = true;
    err = R_DMAC_Open(&g_at_dmac_ctrl, &g_at_dmac_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: DMAC open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_dmac_open = true;
    R_DMAC_Enable(&g_at_dmac_ctrl);
    R_GPT_Start(&g_at_gpt_ctrl);

    print("analog-test: DAC8 ch0 ramping into ACMPLP0 IVREF0 at "); printDec0(RAMP_HZ); print(" Hz. ");
    print("Apply a known voltage to CMPIN0 (P100), watch VCOUT (P110). "); printCr();
    print("C0MON now reads: "); printDec0(R_ACMPLP->COMPMDR_b.C0MON); printCr();
    print("`analog-test-stop` to end."); printCr();
}

// opamp0-test — ( -- ) turn on OPAMP0 as a plain 3-pin differential amp.
// AMP0+ = P000, AMP0- = P001, AMP0O = P002 — the only one of the four
// on-chip OPAMP units with all three pins exposed on this board. No
// DMA/ramp involved: feed it real test voltages on AMP0+/AMP0- and scope
// AMP0O directly. These three pins double as AN000/AN001/AN002, the old
// hex-board ISNS_B/ISNS_C/VIN_S scan (Board/adc.c) — moot now the hex
// board is abandoned, but Table 35.12 already forbids scanning AN000/
// AN001 on the ADC while OPAMP0 uses them as inputs, so nothing new
// breaks by reusing them here.
void analog_test_cli_opamp0(void) {
    R_BSP_MODULE_START(FSP_IP_OPAMP, 0U);   // MSTPD31 = 0

    R_IOPORT_Open(&g_at_ioport_ctrl, NULL);
    // HWM 19.5.4: any analog function (input or output) needs PMR=0,
    // PDR=0 (general input port) with only ASEL set — same treatment
    // already proven on CMPIN0 (input) and DAC12's P014 (output).
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_00, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0+
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_01, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0-
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_02, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0O

    // AMPMC (38.2.1): leave AMPSP=0 (low-power mode) and AMPPC0=0 (no
    // precharge) — written explicitly even though both are the reset
    // default, same documentation style as ACMPLP's step-by-step setup.
    R_OPAMP->AMPMC = 0x00U;
    // AMPC (38.2.4): AMPE0=1 and IREFE=1 in the same write — Figure 38.2's
    // transition (4), straight from Status 1 to Status 3 in one step.
    // AMPTRM is left at its reset default (00b = software trigger mode),
    // so no activation trigger is involved.
    R_OPAMP->AMPC = 0x81U;   // bit0 AMPE0=1, bit7 IREFE=1

    print("analog-test: OPAMP0 enabled, settling ("); printDec0(650U); print(" us min, Table 48.68 Tstd3). ");
    print("Feed AMP0+ (P000) and AMP0- (P001), scope AMP0O (P002). `analog-test-stop` to end."); printCr();
    after(msec(1), analog_test_opamp0_continue);
}

static void analog_test_opamp0_continue(void) {
    print("analog-test: OPAMP0 AMPMON0 reads "); printDec0(R_OPAMP->AMPMON_b.AMPMON0);
    print(" (1 = operating)."); printCr();
}

// opamp0-sweep — ( -- ) jumper DAC12 (P014) to AMP0+ (P000) first. Steps
// DAC12 through its full range and prints the expected setpoint (DAC12's
// own output in mV, via the ADC's tracked AVCC0 reference) against the
// measured OPAMP0 output (AMP0O/P002, read back over the ADC's existing
// AN002 scan channel — no extra wiring, see Board/adc.c's ADC_PIN_MASK).
// One line per step; paste the output into a spreadsheet to plot the
// transfer curve. Static DAC12 writes, no GPT1/DMAC ramp — that hardware
// is explicitly freed first since the shared DMA destination pointer
// would otherwise fight these direct writes to the same register.
#define OPAMP0_SWEEP_STEPS 33U   // 0..4095 in 33 points, ~128 codes apart

static uint32_t g_opamp0_sweep_i;

void analog_test_cli_opamp0_sweep(void) {
    analog_test_stop_timers();   // free GPT1/DMAC/DAC12 and clear OPAMP0 first

    R_BSP_MODULE_START(FSP_IP_DAC, 0U);
    R_DAC->DACR     = 0x4FU;
    R_DAC->DAVREFCR = 0x01U;

    R_BSP_MODULE_START(FSP_IP_OPAMP, 0U);

    R_IOPORT_Open(&g_at_ioport_ctrl, NULL);
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_14, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // DAC12
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_00, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0+
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_01, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0-
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_02, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0O

    R_OPAMP->AMPMC = 0x00U;
    R_OPAMP->AMPC  = 0x81U;   // AMPE0=1, IREFE=1

    print("analog-test: opamp0-sweep - confirm DAC12 (P014) is jumpered to AMP0+ (P000). ");
    printDec0(OPAMP0_SWEEP_STEPS); print(" steps across 0-4095. dac12_mv vs amp0o_mv:"); printCr();

    g_opamp0_sweep_i = 0U;
    after(msec(1), analog_test_opamp0_sweep_step);   // let OPAMP0's Tstd3 (650us) elapse first
}

static void analog_test_opamp0_sweep_step(void) {
    uint16_t dac_code = (uint16_t) ((4095UL * g_opamp0_sweep_i) / (OPAMP0_SWEEP_STEPS - 1U));
    R_DAC->DADR[0] = dac_code;
    after(msec(1), analog_test_opamp0_sweep_read);   // Tset1 max 750us (low-power OPAMP), covers the DAC too
}

static void analog_test_opamp0_sweep_read(void) {
    uint16_t dac_code = (uint16_t) ((4095UL * g_opamp0_sweep_i) / (OPAMP0_SWEEP_STEPS - 1U));
    uint32_t dac_mv  = (uint32_t) dac_code * adc_vref_get() / 4095UL;
    uint32_t amp0_mv = adc_read_an_mv(2U);   // AN002 = AMP0O

    printDec0(dac_mv); print(", "); printDec0(amp0_mv); printCr();

    g_opamp0_sweep_i++;
    if (g_opamp0_sweep_i >= OPAMP0_SWEEP_STEPS) {
        print("analog-test: opamp0-sweep done."); printCr();
        return;
    }
    after(msec(1), analog_test_opamp0_sweep_step);
}

// dac-status — ( -- ) MSTP/register state for DAC12, DAC8, ACMPLP, and a
// measured GPT1 rate (two GTCNT reads a known delay apart — settles what
// GPT1 is actually counting at, rather than trusting the source_div math).
void analog_test_cli_status(void) {
    print("DAC12: MSTPD20="); printDec0(R_MSTP->MSTPCRD_b.MSTPD20); print(" (0=clocked)");
    print("  DAE="); printDec0(R_DAC->DACR_b.DAE);
    print("  DAOE0="); printDec0(R_DAC->DACR_b.DAOE0);
    print("  DADR[0]="); printDec0(R_DAC->DADR_b[0].DADR);
    print("  DAVREFCR="); printDec0(R_DAC->DAVREFCR); print(" (0=no ref selected, output stays 0 regardless of DADR)"); printCr();

    print("DAC8:  MSTPD19="); printDec0((unsigned) ((R_MSTP->MSTPCRD >> 19U) & 1U)); print(" (0=clocked)");
    print("  DACE0="); printDec0(R_DAC8->DAM_b.DACE0);
    print("  DACS[0]="); printDec0(R_DAC8->DACS[0]); printCr();

    print("ACMPLP: MSTPD29="); printDec0(R_MSTP->MSTPCRD_b.MSTPD29); print(" (0=clocked)");
    print("  C0ENB="); printDec0(R_ACMPLP->COMPMDR_b.C0ENB);
    print("  C0WDE="); printDec0(R_ACMPLP->COMPMDR_b.C0WDE);
    print("  C0VRF="); printDec0(R_ACMPLP->COMPMDR_b.C0VRF);
    print("  C0MON="); printDec0(R_ACMPLP->COMPMDR_b.C0MON); printCr();
    print("  IVCMP0="); printDec0(R_ACMPLP->COMPSEL0_b.IVCMP0);
    print("  IVREF0="); printDec0(R_ACMPLP->COMPSEL1_b.IVREF0);
    print("  C0OE="); printDec0(R_ACMPLP->COMPOCR_b.C0OE);
    print("  C0OP="); printDec0(R_ACMPLP->COMPOCR_b.C0OP); printCr();

    print("OPAMP0: MSTPD31="); printDec0(R_MSTP->MSTPCRD_b.MSTPD31); print(" (0=clocked)");
    print("  AMPE0="); printDec0(R_OPAMP->AMPC_b.AMPE0);
    print("  IREFE="); printDec0(R_OPAMP->AMPC_b.IREFE);
    print("  AMPMON0="); printDec0(R_OPAMP->AMPMON_b.AMPMON0); print(" (1=operating)"); printCr();

    // period_counts wraps constantly — two timed reads just land on two
    // unrelated points in [0, period) and prove nothing either way. Count
    // actual wraps (a decrease between consecutive polls) instead: zero
    // wraps means it's very likely not running; any wraps means it is.
    // NOTE: this loop still busy-polls briefly — a real TEA-philosophy
    // violation (see conversation), kept short and bounded since dac-status
    // is an on-demand diagnostic, not part of any recurring/hot path. 3000
    // iterations of a tight register read is enough to catch a wrap at
    // anything from a few kHz up; shrunk down from 50000 to minimize it.
    uint32_t wraps = 0U, last = R_GPT1->GTCNT;
    for (uint32_t i = 0U; i < 3000U; i++) {
        uint32_t now = R_GPT1->GTCNT;
        if (now < last) wraps++;
        last = now;
    }
    print("GPT1: "); printDec0(wraps); print(" wraps seen over 3000 GTCNT polls (period_counts=");
    printDec0(GPT1_PERIOD); print("). Zero here means GPT1 is not counting."); printCr();
}
