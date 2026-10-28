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
#include "power_sense.h"
#include "vin_sense.h"
#include "adc_sync.h"
#include "cli.h"
#include "printers.h"
#include "tea.h"

static ioport_instance_ctrl_t g_at_ioport_ctrl;
// R_IOPORT_Open(&g_at_ioport_ctrl, NULL) was passed NULL in place of a real
// config at all five call sites below. IOPORT_CFG_PARAM_CHECKING_ENABLE is
// 0 project-wide, so FSP's own NULL guard on p_cfg is compiled out, and
// r_ioport_pins_config() dereferences it unconditionally: p_pin_data->
// number_of_pins and p_pin_data->p_pin_cfg_data are read straight from
// address 0 (this chip's vector table / the DFU bootloader's own bytes),
// then looped that many times writing garbage into PFS registers across
// the chip. Confirmed against ra/fsp/src/r_ioport/r_ioport.c directly.
// An explicit, valid, zero-pin config avoids the whole thing — individual
// PinCfg calls below still do the actual per-pin configuration.
static const ioport_cfg_t g_at_ioport_cfg = {
    .number_of_pins = 0U,
    .p_pin_cfg_data = NULL,
    .p_extend       = NULL,
};
static void analog_test_dac8_cmp_continue(void);
static void analog_test_opamp0_continue(void);
static void analog_test_dmac_rearm(void);
static void analog_test_wire_sweep_step(void);
static void analog_test_wire_sweep_sample(void);

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

// Group B's (VIN) ADC trigger offset from GPT1's overflow — see the
// compare_match_value/compare_match_status comment below for why this
// exists and why VIN, not Vsense/Vdrain, sits near the edge. 5us is a
// starting guess (same spirit as wsettle!/wsample-ms!: tune against the
// bench, not a validated number), comfortably clear of group A's own
// trigger at the half-period mark (96 counts, ~32us).
#define ADC_VIN_OFFSET_US_DEFAULT 5U
#define ADC_VIN_OFFSET_COUNTS (3U * ADC_VIN_OFFSET_US_DEFAULT)  // GPT1_HZ is 3 counts/us

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
// DAC8 feeds ACMPLP0's carrier (IVREF0), compared continuously against
// CMPIN0 (dac8-cmp). Any CMPIN0 value inside the carrier's own sweep range
// gets crossed every cycle, generating periodic edges regardless of what
// that value is — DAC12=0 sitting exactly at a full-scale carrier's own
// minimum was producing a steady trickle of boundary-grazing edges rather
// than true silence. Narrowing the carrier to a band that excludes the
// "off" test value entirely (CMPIN0 then sits below — or above — the
// whole range, the comparator output stops toggling, and the cap-coupled
// RC stage downstream — which only responds to edges, not DC level —
// settles to its true rest state) fixes that. The "on" test value needs
// to land inside this band to get real toggling/retriggering; outside it
// on either side just settles to a steady, non-toggling state again.
// Values below are DAC8 8-bit codes (0-255), not DAC12's — DAC12's own
// ramp (dac12-ramp) is unrelated and keeps the full 0-4095 range.
//
// Widened from 50-200 to 5-250 for mapping the wire's characteristic in
// digital mode at the higher error-amp gain (R2=470k): the narrow band
// above was tuned for clean on/off silence at the trigger-pulse test
// values, but at the higher gain AMP0O's excursion across the wire's
// full onset-to-saturation range is wider than that ~3V band could
// represent — confirmed on the bench, the narrow carrier cut the sweep
// off (comparator settling to steady "off") before the wire's real
// saturation plateau ever showed up. This trades back some of that
// clean-silence margin for headroom; if trigger-pulse testing needs the
// tighter band again later, these two constants are the only thing that
// needs to change back.
#define DAC8_CARRIER_MIN 5U     // ~102 mV at AVCC0 ~5190 mV (opamp0-sweep's measured vref)
#define DAC8_CARRIER_MAX 250U   // ~5088 mV at AVCC0 ~5190 mV

static void build_ramp(void) {
    for (uint32_t k = 0; k < RAMP_STEPS; k++) {
        uint32_t tri = (k < RAMP_STEPS / 2U) ? k : (RAMP_STEPS - k);
        uint32_t v12 = (4095U * tri) / (RAMP_STEPS / 2U);
        g_ramp12[k] = (uint16_t) v12;
        g_ramp8[k]  = (uint8_t) (DAC8_CARRIER_MIN + (DAC8_CARRIER_MAX - DAC8_CARRIER_MIN) * tri / (RAMP_STEPS / 2U));
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
    // compare_match_value[0] (GTCCRA) gives Board/adc_sync.c's ADC group A
    // (Vsense/Vdrain, read together) its trigger at the half-period mark
    // rather than on GPT1's raw overflow. The overflow is the exact same
    // event that steps the DAC8/DAC12 ramp table (g_at_dmac_ext below) —
    // i.e. the real circuit's own switching edge — so a group-A conversion
    // triggered by the overflow starts its sample-and-hold right as Vsense
    // is transitioning/ringing. Bench-confirmed on the scope: a sharp
    // overshoot/undershoot sits right at that edge, and occasional pstatus
    // reads (97-222mV against a ~55mV baseline) were catching it. Sitting
    // at the half-period mark instead keeps Vsense/Vdrain's own read as far
    // from either edge as this carrier allows.
    //
    // compare_match_value[1] (GTCCRB) gives group B (VIN) its own trigger,
    // offset only a few us from GPT1's overflow — VIN is read first in
    // time, same spirit as reading it before Vsense/Vdrain in
    // power_sense_read_group() below, since it's a slow, edge-insensitive
    // signal that doesn't need the settling margin group A's own offset
    // exists for. Needs its OWN event regardless of ordering: per HWM
    // 35.3.2 ("if the conversion processes in group A and B occur at the
    // same time, those conversions cannot be controlled separately") group
    // B would never convert if both groups shared one event — stuck at 0
    // forever, not a one-off glitch. Harmless to the GTIOCB-less words that
    // share this same cfg (gpt1-only, dmac-only, dac12-ramp): nothing reads
    // GTCCRB or enables GTIOCB output for them.
    // ADC_VIN_OFFSET_US below is that few-us offset; see aoffset-us! to
    // retune it against the scope without reflashing (it now pokes GTCCRB,
    // not GTCCRA — see that word's own comment).
    //
    // compare_match_status must have the matching bit set for a
    // compare_match_value[] entry to actually get written to hardware at
    // Open — found by reading r_gpt.c's r_gpt_init_compare_match_channel():
    // with a status bit left 0, that GTCCR register is never written, so it
    // keeps its power-on-reset value (HWM register summary: GTCCRA/B reset
    // to FFFFFFFFh) — a value GTCNT (0..GPT1_PERIOD-1 here) can never reach,
    // so the compare-match event it's supposed to feed would never fire.
    // The private mask constants (GPT_PRV_COMPARE_MATCH_A_MASK=0x01,
    // _B_MASK=0x02) aren't exported by the FSP header, hence the literals.
    .compare_match_value  = { GPT1_PERIOD / 2U, ADC_VIN_OFFSET_COUNTS + 1U },
    .compare_match_status = 0x03U,   // bit0 = GTCCRA, bit1 = GTCCRB enabled
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

// Stops GPT1/DMAC (the carrier) and any pending sweep/rearm actions, but
// leaves OPAMP0 untouched — split out of analog_test_stop_timers() so
// dac8-stop can silence the carrier alone for analog-mode testing, where
// OPAMP0 needs to keep running but the comparator/DAC8 carrier doesn't
// (and is a real noise source per the gate-off bench test, worth
// silencing when it's not actually needed).
static void analog_test_stop_carrier(void) {
    // Guarded on our own open-tracking, not FSP's: BSP_CFG_PARAM_CHECKING_
    // ENABLE is 0 project-wide, so R_GPT_Stop/Close and R_DMAC_Disable/
    // Close have no "was this opened?" check compiled in at all — calling
    // them on a control block that was never opened (e.g. opamp0-sweep
    // run as the very first command after a cold boot) dereferences a
    // NULL p_reg/p_cfg and hard-faults the MCU before anything can print.
    if (g_at_dmac_open) { R_DMAC_Disable(&g_at_dmac_ctrl); R_DMAC_Close(&g_at_dmac_ctrl); g_at_dmac_open = false; }
    if (g_at_gpt_open)  { R_GPT_Stop(&g_at_gpt_ctrl); R_GPT_Close(&g_at_gpt_ctrl); g_at_gpt_open = false; }
    stop_te(analog_test_dmac_rearm);
    stop_te(analog_test_wire_sweep_step);
    stop_te(analog_test_wire_sweep_sample);
    // GPT1's overflow is about to stop firing (or already has) — revert the
    // ADC to continuous-scan before it goes stale waiting on a trigger that
    // no longer comes. Safe to call even if adc_sync_start() was never
    // reached this session (e.g. dac8-stop before dac8-cmp ever ran): it's
    // just Close+adc_init(), same idempotent end state either way.
    adc_sync_stop();
}

static void analog_test_stop_timers(void) {
    analog_test_stop_carrier();
    // R_BSP_MODULE_START is idempotent (just clears the MSTP bit), so this
    // is safe to call every time. It is NOT safe to skip: if OPAMP's own
    // module clock (MSTPD31) was never started (e.g. dac12! run as the
    // first analog-test command after a cold boot, before opamp0-test/
    // opamp0-sweep ever ran), writing AMPC below while the clock is gated
    // can stall the bus outright rather than fault or drop silently — the
    // same class of hazard already documented for the RTC in clocks.c
    // (FSP_HARDWARE_REGISTER_WAIT spinning forever on a stopped module).
    // A genuine bus stall wedges the core hard enough that nothing else
    // can run (no fault captured, no USB activity, no LED) and even a
    // software reset may not be reachable — matching the observed lockup
    // exactly.
    R_BSP_MODULE_START(FSP_IP_OPAMP, 0U);
    R_OPAMP->AMPC = 0x00U;   // AMPE0=0, IREFE=0 — safe to call even if opamp0-test was never run
}

// TRANSFER_MODE_REPEAT is not actually infinite: DMCRB (the repeat counter)
// decrements once per full source-table cycle and the transfer genuinely
// ends when it reaches 0 (HWM 16.4.1: "the value of DMACm.DMCRB changes
// from 1 to 0, DMA transfer ends"). num_blocks=0 maps to the maximum,
// 65536 (HWM 16.2.4), so a continuous ramp runs for
// 65536 * RAMP_STEPS/RAMP_HZ = 65536 * 4.096 ms =~ 268 s (~4.5 min) before
// DMAC stops itself — confirmed on the bench: dac8-cmp's carrier went
// silent after a few minutes of otherwise-untouched operation. R_DMAC_
// Enable() alone cannot resume it afterward either: it only sets DMCNT.DTE
// = 1 (r_dmac_prv_enable), it does not reload DMCRB. The fix is a full
// Close+Open+Enable re-arm, done periodically well inside the 268 s
// window so DMCRB never actually reaches 0 in practice. GPT1 itself needs
// no equivalent treatment — it is a plain free-running periodic timer with
// no block/repeat counter to exhaust.
#define DMAC_REARM_MS 60000U   // well under the ~268 s DMCRB exhaustion limit

static void analog_test_dmac_rearm(void) {
    if (!g_at_dmac_open) return;   // stopped since this was scheduled — nothing to re-arm
    R_DMAC_Disable(&g_at_dmac_ctrl);
    R_DMAC_Close(&g_at_dmac_ctrl);
    R_DMAC_Open(&g_at_dmac_ctrl, &g_at_dmac_cfg);   // fresh DMCRB, reloaded from g_at_dmac_info.num_blocks
    R_DMAC_Enable(&g_at_dmac_ctrl);
    after(msec(DMAC_REARM_MS), analog_test_dmac_rearm);
}

void analog_test_cli_stop(void) {
    analog_test_stop_timers();
    print("analog-test: stopped"); printCr();
}

// dac8-stop — ( -- ) stop just the DAC8/ACMPLP0 carrier (GPT1+DMAC, the
// comparator), leaving OPAMP0 running untouched — unlike analog-test-stop,
// which stops both together. For analog-mode testing: dac8-cmp is the
// only word that turns OPAMP0 on, but it also starts the carrier, which
// analog mode doesn't use and which runs continuously regardless of
// anything else (confirmed a real noise source on the bench, independent
// of the gate's own state) — worth silencing once OPAMP0 is up and the
// carrier/comparator path is no longer needed.
void analog_test_cli_dac8_stop(void) {
    analog_test_stop_carrier();
    R_DAC8->DAM_b.DACE0 = 0U;
    R_ACMPLP->COMPMDR_b.C0ENB = 0U;
    print("analog-test: DAC8/ACMPLP0/carrier stopped, OPAMP0 left running."); printCr();
}

// cmpfir! — ( n -- ) ACMPLP0's own digital resampling stage on VCOUT
// (COMPFIR.C0FCK, 39.2.2): 0 bypass (no sampling, reset default — what
// we've been running with the whole time this file has existed), 1 sample
// at PCLKB, 2 at PCLKB/8, 3 at PCLKB/32. No analog hysteresis exists on
// this comparator at all (checked every COMPMDR/COMPFIR/COMPOCR/COMPSEL0/1
// bit — nothing resembling one), so this resampling stage is the only
// on-chip lever against the near-threshold chatter seen on the bench: a
// toggle faster than the chosen resample period gets smoothed out before
// it ever reaches VCOUT/the gate. Safe to change live while dac8-cmp is
// running — this just reselects the output's own sampling clock, not
// C0ENB or anything the ACMPLP setup sequence (Table 39.3) requires
// quiesced first.
void analog_test_cli_cmpfir_set(void) {
    Cell v = ret();
    if (v < 0 || v > 3) { print("cmpfir!: 0 bypass, 1 PCLKB, 2 PCLKB/8, 3 PCLKB/32"); printCr(); return; }
    R_ACMPLP->COMPFIR_b.C0FCK = (uint8_t) v;
    print("cmpfir: C0FCK="); printDec0((unsigned) v); printCr();
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

    // Load the first sample before enabling the output driver below —
    // DADR[0] otherwise still holds whatever a previous dac12-ramp/dac12!/
    // opamp0-sweep run last left it at, and DAOE0 going live exposes that
    // stale value on P014 for one visible step before the DMA's first
    // transfer overwrites it. Confirmed on scope: a brief glitch to a
    // stale level right as the pin goes live, before settling.
    R_DAC->DADR[0] = g_ramp12[0];

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
    R_IOPORT_Open(&g_at_ioport_ctrl, &g_at_ioport_cfg);
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
    after(msec(DMAC_REARM_MS), analog_test_dmac_rearm);   // DMCRB exhausts after ~268s otherwise

    print("analog-test: DAC12 (P014) ramping at "); printDec0(RAMP_HZ); print(" Hz, "); printDec0(RAMP_STEPS);
    print(" steps. Scope P014. `analog-test-stop` to end."); printCr();
}

// dac12! — ( mv -- ) hold DAC12 (P014) at one fixed voltage, no ramp — for
// manual forward-path bring-up (pick a setpoint, watch the FET/wire/sense
// resistor at that one point) rather than sweeping. Deliberately does NOT
// call analog_test_stop_timers(): GPT1/DMAC only ever need stopping here to
// protect against dac12-ramp fighting this same DADR[0] write, which is a
// combination nobody actually wants (same pin, opposite purposes). Calling
// the shared stop was also killing dac8-cmp's carrier and clearing OPAMP0's
// AMPC on every call — exactly the combination the real closed-loop test
// needs running continuously while dac12! pulses the setpoint (e.g. a `for
// ... next` macro alternating dac12! between two values). If dac12-ramp
// genuinely is running when this is called, the static write here just
// gets overwritten by the next DMA cycle (~64us later) — self-evident on
// the bench, not worth defending against for a combination nobody wants.
// Shared by dac12! and wire-sweep below — same body, just not popping the
// data stack, so a scheduled sweep step can call it directly.
static void analog_test_dac12_set_mv(uint32_t mv) {
    uint32_t vref = adc_vref_get();
    if (mv > vref) mv = vref;

    uint32_t code = mv * 4095UL / vref;

    R_BSP_MODULE_START(FSP_IP_DAC, 0U);
    // Load the target value before enabling the output driver (DAOE0)
    // below — otherwise DADR[0] still holds whatever a previous test left
    // it at, and that stale value briefly appears on P014 the instant
    // DAOE0 goes live, before this write overwrites it. Confirmed on
    // scope: a visible glitch-then-settle instead of a clean single step.
    R_DAC->DADR[0] = (uint16_t) code;
    R_DAC->DACR     = 0x4FU;
    R_DAC->DAVREFCR = 0x01U;
    R_IOPORT_Open(&g_at_ioport_ctrl, &g_at_ioport_cfg);
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_14, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);
}

void analog_test_cli_dac12_set(void) {
    Cell mv = ret();
    analog_test_dac12_set_mv((uint32_t) mv);

    // No confirmation print, deliberately: this word is meant to be called
    // from tight timed loops (e.g. a `for ... next` CLI macro alternating
    // two voltages for a pulse test), and USB/UART transmission time would
    // perturb exactly the timing being tested. Check the result with
    // dac-status or a scope, not the console.
}

// dac8-cmp — ( -- ) put a known test voltage on CMPIN0 (P100) first,
// then watch VCOUT (P110) for a clean transition as DAC8 ramps through it.
// Also enables OPAMP0 itself (same as opamp0-test): CMPIN0 is meant to be
// fed by AMP0O in the real trigger/closed-loop path, so this test owns
// turning that on rather than leaving it as a separate step someone has
// to remember to run first — a standalone test shouldn't depend on
// knowledge the user has to carry in their head. Only the external
// jumpers (what's physically wired to AMP0+/AMP0-/AMP0O) stay the user's
// responsibility; the peripheral itself is this word's job.
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

    // OPAMP0 (see opamp0-test for the register-level rationale): AMPE0+
    // IREFE together, same Status-1-to-Status-3 transition. Its 650us
    // Tstd3 settle is comfortably covered by the Tcmp wait below.
    R_BSP_MODULE_START(FSP_IP_OPAMP, 0U);
    R_OPAMP->AMPMC = 0x00U;
    R_OPAMP->AMPC  = 0x81U;

    R_IOPORT_Open(&g_at_ioport_ctrl, &g_at_ioport_cfg);
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_00, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0+
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_01, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0-
    R_IOPORT_PinCfg(&g_at_ioport_ctrl, BSP_IO_PORT_00_PIN_02, (uint32_t) IOPORT_CFG_ANALOG_ENABLE);  // AMP0O
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
    // Default to the most aggressive VCOUT resample (COMPFIR.C0FCK=3,
    // PCLKB/32 = 750kHz here, ~1.33us added latency) rather than bypass.
    // ACMPLP has no analog hysteresis at all (checked every register), so
    // this digital resample is the only on-chip defense against near-
    // threshold chatter — confirmed on the bench across 0-3: bypass/PCLKB
    // chatter heavily, PCLKB/32 collapses it to clean single transitions
    // at a mid/high setpoint (500mV dac12!). At low setpoint (~100mV) the
    // pulses still jitter pulse-to-pulse even at this setting — real
    // instability near the carrier's noisy low end, not something this
    // register can fully clean up. `cmpfir!` can still override live;
    // expect low-setpoint calibration reads to need their own firmware-
    // side averaging/filtering on top of this, not rely on it alone.
    R_ACMPLP->COMPFIR_b.C0FCK = 3U;
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
    // Inverted: this VCOUT feeds a single 74HCT14 Schmitt stage rather than
    // two cascaded ones (matching the production 74LVC2G17's one-gate-per-
    // channel topology). One inversion from that gate needs pre-correcting
    // here so the final polarity at the gate still lands right-side up.
    R_ACMPLP->COMPOCR_b.C0OP = 1U;                        // inverted

    fsp_err_t err = R_GPT_Open(&g_at_gpt_ctrl, &g_at_gpt_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: GPT open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_gpt_open = true;
    err = R_DMAC_Open(&g_at_dmac_ctrl, &g_at_dmac_cfg);
    if (err != FSP_SUCCESS) { print("analog-test: DMAC open failed, err="); printDec0((unsigned) err); printCr(); return; }
    g_at_dmac_open = true;
    R_DMAC_Enable(&g_at_dmac_ctrl);
    R_GPT_Start(&g_at_gpt_ctrl);
    after(msec(DMAC_REARM_MS), analog_test_dmac_rearm);   // DMCRB exhausts after ~268s otherwise

    // GPT1 is now actually counting, so its overflow event (Board/adc_sync.c's
    // ELC links) starts firing — safe to switch the ADC into triggered
    // group-scan mode now, not before. Reverted by dac8-stop/analog-test-stop.
    adc_sync_start();

    print("analog-test: DAC8 ch0 ramping into ACMPLP0 IVREF0 at "); printDec0(RAMP_HZ); print(" Hz, OPAMP0 enabled. ");
    print("CMPIN0 (P100) gets whatever AMP0O (P002) is wired to drive, watch VCOUT (P110). "); printCr();
    print("C0MON now reads: "); printDec0(R_ACMPLP->COMPMDR_b.C0MON); printCr();
    print("Vsense/Vdrain now hardware-triggered off the carrier (trig-status); scope P112 for the sample-ready marker."); printCr();
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

    R_IOPORT_Open(&g_at_ioport_ctrl, &g_at_ioport_cfg);
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

// opamp0-sweep — retired. It jumpered DAC12 (P014) to AMP0+ (P000) and read
// OPAMP0's output (AMP0O/P002) back over the ADC's AN002 scan channel; that
// channel is gone now the board's analog inputs are consolidated to VIN/
// Vdrain/Vsense (Board/adc.c's ADC_PIN_MASK). OPAMP0 was part of this
// board's original analog circuit, superseded by the digital (sr595) muscle
// drive and the closed-loop VIN/Vdrain/Vsense sensing it uses instead.
void analog_test_cli_opamp0_sweep(void) {
    print("opamp0-sweep: retired -- AN002 is no longer an ADC scan channel (see Board/adc.c). "); printCr();
    print("  Scope AMP0O/P002 directly if OPAMP0's own transfer curve is still wanted."); printCr();
}

// wire-sweep — ( -- ) assumes dac8-cmp is already running (carrier + ADC
// group-scan trigger live, see Board/adc_sync.c) and a muscle wire is wired
// into the real closed-loop path, not opamp0-sweep's bench jumper. Steps
// DAC12 (the setpoint) from 0 to WIRE_SWEEP_MV_MAX in g_wire_sweep_step_mv
// increments; at each setpoint, settles, then takes several hardware-
// triggered reads of Vsense/Vdrain/R_wire, printing one row per setpoint —
// mv, R_wire avg/min/max (the sigmoid plus its own per-setpoint stability/
// spread), Vsense avg, Vdrain avg. Shuts the gate off (dac12=0) when done
// or stopped, rather than leaving the wire energized at whatever the last
// setpoint was. 1000mV upper bound (lowered from an original 1500mV once
// bench sweeps showed the transition fully resolved well under that):
// enough to pull the full sigmoidal transition out without wasting time/
// power above where it saturates.
//
// Step size, settle time, sample count, and sample spacing are all
// runtime-settable (wstep!/wsettle!/wsamples!/wsample-ms!), not fixed
// constants — the first version used a 50ms settle sized only for the
// carrier's own electrical cycle (a dozen or so 4.096ms periods), which
// ignores that the wire itself has real thermal mass: changing the average
// duty cycle takes genuine time to reach a new equilibrium temperature and
// resistance, far slower than the electrical switching. Confirmed wrong on
// the bench: with the old 50ms/8-sample settings every step's R_wire min
// was exactly 0 and consecutive steps swung wildly (e.g. 278160 then
// 175167 mOhm) — the wire was still mid-transient at every single sample,
// never actually reaching the setpoint's real steady state before the
// sweep moved on. No single correct settle time is known yet; the defaults
// (50mV step, 500ms settle, 16 samples, 20ms apart) are a starting point to
// tune against the bench, not validated numbers. A fine step (wstep! 1)
// with a short settle and 1 sample (wsettle! 20, wsamples! 1) reproduces
// the user's own manual 1mV/20ms bench recipe, which resolved the SMA
// phase-transition region far better than the coarse default — useful for
// characterizing a narrow transition once wire-sweep has located it, though
// a 1mV/1500mV run is 1500 steps and gets slow if settle/samples are left
// at the coarse defaults.
#define WIRE_SWEEP_MV_MAX     1000U

static uint32_t g_wire_sweep_settle_ms  = 500U;
static uint32_t g_wire_sweep_sample_ms  = 20U;
static uint32_t g_wire_sweep_samples    = 16U;
static uint32_t g_wire_sweep_step_mv    = 50U;   // 21 points, 0-1000mV at the default

// wstep! — ( mv -- ) wire-sweep's setpoint increment. Runtime-settable
// rather than the fixed 50mV it started as: a 1mV step with wsettle! at
// 20ms and wsamples! at 1 reproduced the user's own bench recipe (manual
// 1mV/20ms increments) that resolved the SMA phase-transition region far
// better than the coarse 50mV sweep — at 1mV/1000mV that's 1000 steps, so
// pair a fine step with a short settle/sample-count or the sweep gets very
// slow. No lower bound beyond >0: 1mV is the DAC's own practical floor.
void analog_test_cli_wstep_set(void) {       // ( mv -- )
    Cell v = ret();
    if (v >= 1 && v <= 1000) g_wire_sweep_step_mv = (uint32_t) v;
}

void analog_test_cli_wsettle_set(void) {     // ( ms -- )
    Cell v = ret();
    if (v >= 0 && v <= 60000) g_wire_sweep_settle_ms = (uint32_t) v;
}

void analog_test_cli_wsample_ms_set(void) {  // ( ms -- )
    Cell v = ret();
    if (v >= 1 && v <= 10000) g_wire_sweep_sample_ms = (uint32_t) v;
}

void analog_test_cli_wsamples_set(void) {    // ( n -- )
    Cell v = ret();
    if (v >= 1 && v <= 200) g_wire_sweep_samples = (uint32_t) v;
}

// CLI word: aoffset-us! ( us -- ) — retune group B's (VIN) ADC trigger
// offset (see the compare_match_value comment above g_at_gpt_ext) without
// a reflash. R_GPT_Open() applies the ADC_VIN_OFFSET_US_DEFAULT starting
// guess through compare_match_value[1] at Open time, which r_gpt.c writes
// to GTCCRB as (compare_match_value[1] - 1) — i.e. the raw register ends up
// holding exactly the desired count (ADC_VIN_OFFSET_COUNTS). This writes
// that same raw count directly (no extra -1: that adjustment is
// compare_match_value's own input convention, not GTCCRB's), so a value
// set here lands identically to one baked in at boot. Only takes effect
// once dac8-cmp (or any GPT1-opening word) has actually opened GPT1 —
// silently does nothing before that, same as every other register-poke CLI
// word in this file.
// Upper bound (31us = 93 counts) keeps it comfortably short of group A's
// own trigger at the half-period mark (96 counts); this is a ceiling on
// what's safe to try, not a claim that values anywhere near it are good.
void analog_test_cli_aoffset_set(void) {     // ( us -- )
    Cell v = ret();
    if (v < 1 || v > 31) {
        print("aoffset-us!: out of range (1-31)"); printCr();
        return;
    }
    R_GPT1->GTCCR[1] = (uint32_t) (3 * v);   // GTCCRB; counts = 3 * us (GPT1_HZ = 3 counts/us)
}

static uint32_t g_wire_sweep_mv;
static uint32_t g_wire_sweep_i;          // ON-phase samples caught so far this step
static uint32_t g_wire_sweep_attempts;   // total samples looked at this step (ON + rejected OFF-phase)
static uint32_t g_wire_sweep_rwire_sum, g_wire_sweep_rwire_min, g_wire_sweep_rwire_max;
static uint32_t g_wire_sweep_vsense_sum, g_wire_sweep_vdrain_sum;

void analog_test_cli_wire_sweep(void) {
    print("analog-test: wire-sweep - dac8-cmp must already be running. ");
    printDec0(WIRE_SWEEP_MV_MAX / g_wire_sweep_step_mv + 1U); print(" steps of "); printDec0(g_wire_sweep_step_mv);
    print("mV, 0-"); printDec0(WIRE_SWEEP_MV_MAX);
    print("mV, settle "); printDec0(g_wire_sweep_settle_ms); print("ms, ");
    printDec0(g_wire_sweep_samples); print(" ON-phase samples "); printDec0(g_wire_sweep_sample_ms);
    print("ms apart (wstep!/wsettle!/wsamples!/wsample-ms! to tune)."); printCr();
    print("mv, rwire_avg_mohm, rwire_min, rwire_max, vsense_avg_mv, vdrain_avg_mv, on_n:"); printCr();

    g_wire_sweep_mv = 0U;
    analog_test_wire_sweep_step();
}

static void analog_test_wire_sweep_step(void) {
    analog_test_dac12_set_mv(g_wire_sweep_mv);

    g_wire_sweep_i          = 0U;
    g_wire_sweep_attempts   = 0U;
    g_wire_sweep_rwire_sum  = 0U;
    g_wire_sweep_rwire_min  = 0xFFFFFFFFUL;
    g_wire_sweep_rwire_max  = 0U;
    g_wire_sweep_vsense_sum = 0U;
    g_wire_sweep_vdrain_sum = 0U;

    after(msec(g_wire_sweep_settle_ms), analog_test_wire_sweep_sample);
}

// Retry spacing when analog_test_wire_sweep_sample() catches P112 low
// (mid-reacquire). The low dip is a small fraction of the ~64us cycle, so
// this almost always succeeds on the very next look; 1ms just has to be
// shorter than the dip is long relative to how often we'd otherwise miss,
// not tuned to the dip's own width.
#define WIRE_SWEEP_MARKER_RETRY_MS 1U

// Per-step attempt budget for finding g_wire_sweep_samples genuinely
// ON-phase samples (RWIRE_ON_THRESHOLD_MV, power_sense.h) — same x10
// multiplier power_cli_rwire_on_median() uses, so a setpoint that's
// genuinely below the wire's conduction threshold (nothing to catch, ever)
// gives up and reports what it has (possibly zero) instead of stalling the
// whole sweep on one step.
#define WIRE_SWEEP_ON_ATTEMPT_MULTIPLIER 10U

static void analog_test_wire_sweep_sample(void) {
    // Gate on the P112 marker (adc_sync.c) instead of trusting
    // g_wire_sweep_sample_ms alone: that interval is chosen for thermal/
    // settle pacing between samples, not to dodge the ~64us hardware scan
    // window, so a sample can still land while group A's Vsense/Vdrain
    // registers are mid-update. Waiting for marker-high means every
    // accepted sample reads a result that's actually settled.
    if (!adc_sync_marker_ready()) {
        after(msec(WIRE_SWEEP_MARKER_RETRY_MS), analog_test_wire_sweep_sample);
        return;
    }

    // Each channel read exactly once (not via rwire_read_mohm(), which
    // would independently re-read vsense/vdrain a second time each) — with
    // ADC_CLEAR_AFTER_READ_ON, a second read of the same channel moments
    // later can land on a since-cleared register before the next ~64us
    // trigger refills it. Same fix as power_cli_status(); see its comment.
    // Group A channels read in hardware scan order (19 then 20) too — see
    // power_cli_status() for why reading them backwards occasionally
    // straddled an in-progress scan.
    uint32_t vsense_mv = vsense_read_mv();
    uint32_t vdrain_mv = vdrain_read_mv();
    uint32_t vin_mv    = vin_read_mv();
    uint32_t rwire = rwire_mohm_from(vin_mv, vdrain_mv, vsense_mv);
    g_wire_sweep_attempts++;

    // ON-phase gate — same floor power_cli_rwire_on_median() validated on
    // the bench (power_sense.h's RWIRE_ON_THRESHOLD_MV). Confirmed the hard
    // way: an un-gated 1mV-step sweep (0-1500mV, 1 sample/step) produced
    // mostly noise — small garbage (hundreds-low-thousands of mOhm) below
    // the wire's conduction threshold, and wild spikes past a million mOhm
    // right at it, because dividing a single-digit-mV Vsense (the idle-
    // phase floor) by a few mV of VIN/Vdrain noise is still just noise. A
    // rejected sample doesn't count against g_wire_sweep_i, so OFF-phase
    // reads never get to vote in the row's avg/min/max the way they used
    // to — no spreadsheet smoothing needed afterward to recover the shape.
    if (vsense_mv > RWIRE_ON_THRESHOLD_MV && rwire != 0U) {
        g_wire_sweep_rwire_sum  += rwire;
        if (rwire < g_wire_sweep_rwire_min) g_wire_sweep_rwire_min = rwire;
        if (rwire > g_wire_sweep_rwire_max) g_wire_sweep_rwire_max = rwire;
        g_wire_sweep_vsense_sum += vsense_mv;
        g_wire_sweep_vdrain_sum += vdrain_mv;
        g_wire_sweep_i++;
    }

    uint32_t max_attempts = g_wire_sweep_samples * WIRE_SWEEP_ON_ATTEMPT_MULTIPLIER;
    if (g_wire_sweep_i < g_wire_sweep_samples && g_wire_sweep_attempts < max_attempts) {
        after(msec(g_wire_sweep_sample_ms), analog_test_wire_sweep_sample);
        return;
    }

    printDec0(g_wire_sweep_mv); print(", ");
    if (g_wire_sweep_i == 0U) {
        print("0, 0, 0, 0, 0, 0");   // never caught an ON-phase sample — not conducting at this setpoint
    } else {
        printDec0(g_wire_sweep_rwire_sum / g_wire_sweep_i); print(", ");
        printDec0(g_wire_sweep_rwire_min); print(", ");
        printDec0(g_wire_sweep_rwire_max); print(", ");
        printDec0(g_wire_sweep_vsense_sum / g_wire_sweep_i); print(", ");
        printDec0(g_wire_sweep_vdrain_sum / g_wire_sweep_i); print(", ");
        printDec0(g_wire_sweep_i);
    }
    printCr();

    g_wire_sweep_mv += g_wire_sweep_step_mv;
    if (g_wire_sweep_mv > WIRE_SWEEP_MV_MAX) {
        analog_test_dac12_set_mv(0U);   // shut the wire off, don't leave it energized at the last setpoint
        print("analog-test: wire-sweep done, dac12 set back to 0."); printCr();
        return;
    }
    analog_test_wire_sweep_step();
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
    print("  C0OP="); printDec0(R_ACMPLP->COMPOCR_b.C0OP);
    print("  C0FCK="); printDec0(R_ACMPLP->COMPFIR_b.C0FCK);
    print(" (0 bypass, 1 PCLKB, 2 PCLKB/8, 3 PCLKB/32)"); printCr();

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
