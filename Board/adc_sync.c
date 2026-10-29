// Board/adc_sync.c — see adc_sync.h.
//
// Two independent, hardware-only (zero CPU/scheduler jitter) ELC links for
// ADC0's two trigger slots:
//
//   GPT3 compare-match A (g_adc_sync_cmp_delay_ext, cleared by ACMPLP0's own
//     rising edge via ELC_GPTA, fires ADC_CMP_DELAY_US_DEFAULT counts later,
//     retunable via cmpdelay-us!) -> ADC0 group A trigger (ELC_AD00)
//                                                          -- Vsense, Vdrain
//   GPT1 compare-match B (ADC_VIN_OFFSET_US_DEFAULT counts past the
//     overflow, retunable via aoffset-us!) -> ADC0 group B trigger (ELC_AD01)
//                                                          -- VIN, read first
//
// Group A used to trigger at a fixed half-period offset off GPT1 (half of
// GPT1_PERIOD, ~32us into the ~64us carrier cycle) — chosen, before this,
// to dodge the switching edge's own overshoot/ringing (bench-confirmed on
// the scope: occasional pstatus reads of 97-222mV against a ~55mV baseline,
// coincident with a sharp transient right at GPT1's raw overflow, which
// also drives the carrier's own DMAC step). That fixed point only works
// when the ON-phase pulse is wide enough to still be open at ~32us —
// wire-sweep's own persistence captures showed pulses as narrow as 25us at
// low setpoints, meaning group A's trigger was firing 7us after the pulse
// had already closed: exactly the "lots of zeroes, especially at the lower
// settings" wire-sweep's own rejection filters were absorbing.
//
// Comparator-edge-referenced triggering fixes that at the root: instead of
// a fixed phase in the carrier cycle, group A's trigger now tracks the
// pulse itself, so it stays valid at any duty down to the wire's actual
// conduction floor. ACMPLP0's rising edge (the pulse turning ON) clears a
// free-running GPT3 via ELC_GPTA; GPT3's own compare-match, ADC_CMP_DELAY_
// US_DEFAULT counts later, becomes ADC0's group-A trigger in place of
// GPT1's old fixed offset. The delay exists to let the switching transient
// settle before the ADC's sample-and-hold opens — scope-confirmed (this
// project's bench capture of Vdrain through a transition) stable by ~5us
// post-edge; ACMPLP0's own cmpfir deglitch filter (PCLKB/32 default,
// ~1.33us) already supplies part of that from the real edge to the ELC
// event GPT3 clears on, so GPT3's own delay only needs to make up the
// remainder — see ADC_CMP_DELAY_US_DEFAULT's own comment below.
//
// GPT3 is free (Board/clocks.c claims GPT0/GPT2, Board/frame_timer.c claims
// GPT7, Board/analog_test.c claims GPT1 for the carrier; GPT3/GPT4 were
// used in three abandoned attempts at a *different* mechanism — see this
// file's own history note below — none of which are live in this build).
// This only uses GPT3's internal compare-match -> ELC path, the same kind
// of link GPT1 already proves out for groups A and B, not the GTIOC pin-
// output path those three attempts got stuck on.
//
// ACMPLP0's own register state already satisfies everything 39.6/39.7 (HWM)
// require for it to emit an ELC event at all: COMPFIR.C0FCK must be
// non-zero (Board/analog_test.c's dac8-cmp sets it to 3, PCLKB/32, for
// deglitching — already satisfies this), and C0EPO/C0EDG (rising edge,
// single-edge detection) are both left at their 0 reset default, which is
// exactly what's wanted here. Nothing about ACMPLP0's own setup needed to
// change for this.
//
// UNTESTED ON HARDWARE as of this commit — compiled, not bench-verified.
// trig-status (adc_sync_cli_status, below) now dumps GPT3's live state and
// the new ELC_GPTA link alongside what it already printed for GPT1; check
// it against the scope before trusting wire-sweep data gathered through
// this path.
//
// Group B does NOT share group A's trigger event (whichever one that is) —
// confirmed wrong on the bench (VIN stuck at 0 forever) when the two groups
// shared the overflow directly. Per the manual (35.3.2): "if the conversion
// processes in group A and B occur at the same time, those conversions
// cannot be controlled separately" — with one shared event, every trigger
// was simultaneous, and group B lost every single time, not occasionally.
// Group A's new comparator-referenced trigger and group B's half-step
// offset off GPT1 stay well clear of each other, so this collision risk
// doesn't reopen.
//
// Confirmed against the manual (35.3.2, ADSTRGR.TRSA/TRSB, Table 35.7):
// RA4M1's ADC140 has exactly two real ELC trigger slots, ELC_AD00/ELC_AD01
// — the FSP header's per-GPT-channel-named ADC_START_SOURCE_GPT_* constants
// (e.g. ADC_START_SOURCE_GPT_A1_A5, suggesting a dedicated GTADTRA1
// register) don't correspond to anything in this chip's own TRSA/TRSB
// encoding; they're generic FSP boilerplate shared across the RA family
// that doesn't apply here, the same trap as DACR's bogus DAE bit and
// R_OPAMP's bogus OS/MS/PS selector found earlier in this project.
//
// This file only wires the ADC0/ADC0_B links above and the scope marker
// below. The ADC's own mode switch (continuous-scan <-> ELC-triggered
// group-scan) lives in Board/adc.c as adc_sync_start()/adc_sync_stop(),
// called from Board/analog_test.c's dac8-cmp/dac8-stop so VIN/status/adcexp
// keep working standalone (see adc.c's own comment) — these ELC links are
// harmless to leave live all the time, since nothing fires until GPT1 is
// actually running.
//
// ── Scope marker: DMA-driven GPIO pulse, not a GPT peripheral output ──────
//
// P112, plain GPIO, driven by two small DMAC transfers instead of a GPT
// channel's own pin-output hardware:
//   ADC0 scan-end       -> DMAC channel 1: write "set P112"   into R_PORT1->PCNTR3
//   GPT1 counter-overflow -> DMAC channel 2: write "clear P112" into R_PORT1->PCNTR3
// So P112 goes HIGH the instant group A's conversion finishes (data valid)
// and goes LOW again the instant the NEXT carrier cycle starts re-acquiring
// (GPT1 overflow, the same event that re-triggers group A) — a level, not a
// pulse, that reads directly as "sample valid" vs "re-acquiring now", with
// zero CPU/ISR involvement either way. PCNTR3 is the exact register
// Board/sr595_hw.c's fast_shift() already uses for fast GPIO writes (low
// 16 bits = per-pin set mask, high 16 bits = per-pin reset mask), and this
// reuses the exact same DMAC-triggered-off-ELC pattern Board/analog_test.c
// already has running for the DAC12/DAC8 ramp — both proven working on
// this hardware, unlike what follows.
//
// This replaces three earlier attempts, all using GPT3/GPT4's own
// peripheral pin-output hardware (TIMER_MODE_ONE_SHOT_PULSE), each
// debugged down to a specific, confirmed root cause in turn:
//   1. P111 = GTIOC3A — double-booked P111/D13 with Board/sr595_hw.c's
//      SRCLK output (plain GPIO).
//   2. P112 = GTIOC3B, driven via compare_match_value/status (writing
//      GTCCRB directly) — compare_match_status nonzero makes R_GPT_Open()
//      write GTBER to DISABLE buffer operation on GTCCRA/B, but
//      TIMER_MODE_ONE_SHOT_PULSE is the driver's own "(fixed buffer
//      operation)" mode: hardware always sources each channel's edge from
//      its buffer pair (GTCCRC/D for A, GTCCRE/F for B), so disabling
//      buffering left GTCCRB's power-on reset value (FFFFFFFFh) as the
//      only thing ever actually compared against. Switched to driving the
//      pulse via duty_cycle_counts instead (the field R_GPT_Open() is
//      actually built to wire up for this mode) — fixed the compare match
//      itself (confirmed via TCFB firing), but:
//   3. gpt3-watch (a tight busy-loop register sample, added when two
//      trig-status calls a beat apart read identically) showed GTCNT
//      cycling 0..29 continuously with CST never 0 — the counter free-runs
//      forever once started instead of stopping between triggers;
//      "one-shot" apparently describes the buffered pulse shape, not the
//      counter. A clear_source fix was tried; gpt3-watch read identically
//      after it. Every further register-level theory (GTICBSR input-
//      capture mode, GTST.ODF/POEG output-disable) checked out clean too —
//      GTIOR decodes to exactly the right "toggle at compare match, output
//      enabled" encoding per the manual's own Table 22.5, yet the pad
//      never moved, while plain GPIO on that same pad (pindir!/pinset!,
//      Board/pins.c) was separately confirmed working cleanly on the
//      scope. GPT's own peripheral-output path for this mode remains
//      unexplained; the DMA approach above sidesteps it entirely by
//      reusing the already-proven GPIO and DMAC-triggered-by-ELC paths
//      instead.

#include "hal_data.h"
#include "r_elc.h"
#include "r_dmac.h"
#include "r_gpt.h"
#include "r_ioport.h"
#include "adc_sync.h"
#include "board_cli.h"
#include "cli.h"
#include "printers.h"
#include "tea.h"   // sysTicks()

// GPT3: comparator-edge-referenced delay stage for group A's ADC trigger —
// see this file's own header comment for the full picture. Cleared by
// ACMPLP0's rising edge (fed in via ELC_PERIPHERAL_GPT_A, chosen because
// nothing in this project uses any of the eight ELC_GPTx slots yet — the
// letter doesn't tie to a specific physical GPT channel, HWM 18.2.3/Figure
// 22.x: any GPT channel's own clear/start/stop source select can listen to
// any of the eight slots, so GPT3 listening to ELC_GPTA is just a free
// choice, not a required pairing), free-running otherwise so GTCNT keeps
// counting between clears. GTCCRA holds the delay in counts at the same
// 3MHz (PCLKD/16) rate GPT1/GPT7 already use successfully on this chip.
//
// ADC_CMP_DELAY_US_DEFAULT: target is the ADC trigger landing ~5us after
// the REAL edge (this project's own scope capture of Vdrain settling
// through a switching transition, confirmed clean by that point). ACMPLP0's
// cmpfir deglitch filter (PCLKB/32, dac8-cmp's own default) already delays
// the ELC event GPT3 clears on by ~1.33us relative to that real edge, so
// GPT3 only needs to add the remainder: 5 - 1.33 ~= 3.7us, 11 counts at
// 3MHz. Rounded up to 4us/12 counts here for a clean default; cmpdelay-us!
// retunes it against the scope without a reflash, same spirit as wstep!/
// wsettle!/aoffset-us! elsewhere in this project — a starting guess, not a
// validated number.
#define ADC_CMP_DELAY_HZ          3000000U   // GPT3 tick: PCLKD(48MHz)/16, matches GPT1_HZ (analog_test.c) and GPT7 (frame_timer.c)
#define ADC_CMP_DELAY_US_DEFAULT  4U
#define ADC_CMP_DELAY_COUNTS      ((ADC_CMP_DELAY_HZ / 1000000U) * ADC_CMP_DELAY_US_DEFAULT)
#define ADC_CMP_DELAY_PERIOD      0xFFFFU   // far longer than any real delay; just bounds the idle case (carrier not running, GPT3 never cleared) to a harmless ~21.8ms free-run instead of relying on clears alone

static gpt_instance_ctrl_t g_adc_sync_cmp_delay_ctrl;
static bool                g_adc_sync_cmp_delay_open;

static const gpt_extended_cfg_t g_adc_sync_cmp_delay_ext = {
    .gtioca = { .output_enabled = false, .stop_level = GPT_PIN_LEVEL_LOW },
    .gtiocb = { .output_enabled = false, .stop_level = GPT_PIN_LEVEL_LOW },
    .start_source      = (gpt_source_t) GPT_SOURCE_NONE,
    .stop_source       = (gpt_source_t) GPT_SOURCE_NONE,
    .clear_source      = (gpt_source_t) GPT_SOURCE_GPT_A,   // cleared by ACMPLP0's rising edge via ELC_GPTA (see g_adc_sync_elc_cfg below)
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
    .compare_match_value  = { ADC_CMP_DELAY_COUNTS, 0U },   // GTCCRA only — this channel drives one event, not two like GPT1
    .compare_match_status = 0x01U,                           // bit0 = GTCCRA enabled (see GPT1's own comment on why this bit is required)
    .p_pwm_cfg = NULL,
    .gtior_setting.gtior = 0U,
    .gtioca_polarity = GPT_GTIOC_POLARITY_NORMAL,
    .gtiocb_polarity = GPT_GTIOC_POLARITY_NORMAL,
};

// No CPU interrupt: GPT3's compare-match feeds ELC_AD00 directly in
// hardware, same as GPT1's own group-A/B triggers.
static const timer_cfg_t g_adc_sync_cmp_delay_cfg = {
    .mode              = TIMER_MODE_PERIODIC,
    .period_counts     = ADC_CMP_DELAY_PERIOD,
    .duty_cycle_counts = 0U,
    .source_div        = TIMER_SOURCE_DIV_16,   // 48MHz/16 = 3MHz, matches ADC_CMP_DELAY_HZ above
    .channel           = 3U,                    // GPT3 — free (GPT0/2/7 claimed, GPT1 is the carrier; see this file's own history note above for why GPT3/4's PIN-output path was abandoned, not relevant to this internal-compare-match-only use)
    .p_callback        = NULL,
    .p_context         = NULL,
    .p_extend          = &g_adc_sync_cmp_delay_ext,
    .cycle_end_ipl     = BSP_IRQ_DISABLED,
    .cycle_end_irq     = FSP_INVALID_VECTOR,
};

static elc_instance_ctrl_t g_adc_sync_elc_ctrl;

static const elc_cfg_t g_adc_sync_elc_cfg = {
    .link = {
        [ELC_PERIPHERAL_ADC0]   = ELC_EVENT_GPT3_CAPTURE_COMPARE_A,
        [ELC_PERIPHERAL_ADC0_B] = ELC_EVENT_GPT1_CAPTURE_COMPARE_B,
        [ELC_PERIPHERAL_GPT_A]  = ELC_EVENT_ACMPLP0_INT,   // feeds GPT3's clear_source above
    },
};

// P112 = port 1, pin 12. PCNTR3's low 16 bits are a per-pin "set" mask,
// high 16 bits a per-pin "reset" mask (same register Board/sr595_hw.c's
// fast_shift() already uses) — one atomic 32-bit write each way.
#define MARKER_PIN_BIT (1UL << 12)
static const uint32_t g_marker_set_value   = MARKER_PIN_BIT;
static const uint32_t g_marker_clear_value = MARKER_PIN_BIT << 16;

static dmac_instance_ctrl_t g_adc_sync_dmac_set_ctrl;
static dmac_instance_ctrl_t g_adc_sync_dmac_clear_ctrl;

static const dmac_extended_cfg_t g_adc_sync_dmac_set_ext = {
    .channel           = 1U,   // DMAC0 is analog_test.c's DAC ramp; 1/2 free
    .irq               = FSP_INVALID_VECTOR,
    .ipl               = BSP_IRQ_DISABLED,
    .offset            = 0,
    .src_buffer_size   = 0U,
    .activation_source = ELC_EVENT_ADC0_SCAN_END,
    .p_callback        = NULL,
    .p_callback_memory = NULL,
    .p_context         = NULL,
};

static const dmac_extended_cfg_t g_adc_sync_dmac_clear_ext = {
    .channel           = 2U,
    .irq               = FSP_INVALID_VECTOR,
    .ipl               = BSP_IRQ_DISABLED,
    .offset            = 0,
    .src_buffer_size   = 0U,
    .activation_source = ELC_EVENT_GPT1_COUNTER_OVERFLOW,
    .p_callback        = NULL,
    .p_callback_memory = NULL,
    .p_context         = NULL,
};

// Both source and destination are fixed (one constant value, one register)
// — nothing to walk/increment, unlike analog_test.c's ramp-table transfer.
// length=1 with TRANSFER_MODE_REPEAT: one write per activation, then
// immediately re-armed for the next one, forever, no CPU involvement.
static transfer_info_t g_adc_sync_dmac_set_info = {
    .transfer_settings_word_b = {
        .dest_addr_mode = TRANSFER_ADDR_MODE_FIXED,
        .repeat_area    = TRANSFER_REPEAT_AREA_SOURCE,
        .irq            = TRANSFER_IRQ_END,
        .chain_mode     = TRANSFER_CHAIN_MODE_DISABLED,
        .src_addr_mode  = TRANSFER_ADDR_MODE_FIXED,
        .size           = TRANSFER_SIZE_4_BYTE,
        .mode           = TRANSFER_MODE_REPEAT,
    },
    .p_src      = (void const *) &g_marker_set_value,
    .p_dest     = (void *) &R_PORT1->PCNTR3,
    .num_blocks = 0U,
    .length     = 1U,
};

static transfer_info_t g_adc_sync_dmac_clear_info = {
    .transfer_settings_word_b = {
        .dest_addr_mode = TRANSFER_ADDR_MODE_FIXED,
        .repeat_area    = TRANSFER_REPEAT_AREA_SOURCE,
        .irq            = TRANSFER_IRQ_END,
        .chain_mode     = TRANSFER_CHAIN_MODE_DISABLED,
        .src_addr_mode  = TRANSFER_ADDR_MODE_FIXED,
        .size           = TRANSFER_SIZE_4_BYTE,
        .mode           = TRANSFER_MODE_REPEAT,
    },
    .p_src      = (void const *) &g_marker_clear_value,
    .p_dest     = (void *) &R_PORT1->PCNTR3,
    .num_blocks = 0U,
    .length     = 1U,
};

static const transfer_cfg_t g_adc_sync_dmac_set_cfg = {
    .p_info   = &g_adc_sync_dmac_set_info,
    .p_extend = &g_adc_sync_dmac_set_ext,
};

static const transfer_cfg_t g_adc_sync_dmac_clear_cfg = {
    .p_info   = &g_adc_sync_dmac_clear_info,
    .p_extend = &g_adc_sync_dmac_clear_ext,
};

static ioport_instance_ctrl_t g_adc_sync_ioport_ctrl;
static const ioport_cfg_t g_adc_sync_ioport_cfg = {    // see Board/analog_test.c for why this can't be NULL
    .number_of_pins = 0U,
    .p_pin_cfg_data = NULL,
    .p_extend       = NULL,
};

static fsp_err_t g_adc_sync_elc_err;
static fsp_err_t g_adc_sync_dmac_set_err;
static fsp_err_t g_adc_sync_dmac_clear_err;
static fsp_err_t g_adc_sync_cmp_delay_err;

// TRANSFER_MODE_REPEAT's block counter (DMCRB) isn't infinite — confirmed
// in Board/analog_test.c's own DMAC (its header comment there walks through
// this in detail): DMCRB defaults to 65536 (num_blocks=0 reads as the max
// of its 16-bit field), and R_DMAC_Enable() alone cannot resume a channel
// once it's exhausted — only a full Close+Open+Enable reloads DMCRB.
// analog_test.c's own channel does `length`=64 transfers per repeat (one
// ramp table walk), so its budget is 64*65536 triggers — at its own ~64us
// GPT1 cadence, ~268s before exhausting, re-armed there every 60s.
// This file's two channels do exactly ONE transfer per repeat (length=1 —
// every activation IS the whole "block"), so the SAME 65536-repeat budget
// only covers 65536 triggers — at the same ~64us ADC cadence, ~4.2s. The
// DTE=0/DTIF=1 seen on the bench (days after this was first written) is
// exactly that: both channels had already run out between being enabled
// at boot and being checked. Re-arming every 1s (a ~4x margin under the
// ~4.2s window, same safety-margin reasoning as analog_test.c's own 60s-
// under-268s) keeps both channels alive indefinitely. This is a periodic
// software tick, not "firmware in the loop" for the marker itself — the
// actual set/clear pulses stay fully hardware-triggered with zero CPU
// involvement; this just tops up a counter a few times a minute.
#define MARKER_DMAC_REARM_MS 1000U

static void adc_sync_dmac_rearm(void) {
    R_DMAC_Disable(&g_adc_sync_dmac_set_ctrl);
    R_DMAC_Close(&g_adc_sync_dmac_set_ctrl);
    g_adc_sync_dmac_set_err = R_DMAC_Open(&g_adc_sync_dmac_set_ctrl, &g_adc_sync_dmac_set_cfg);
    if (g_adc_sync_dmac_set_err == FSP_SUCCESS) g_adc_sync_dmac_set_err = R_DMAC_Enable(&g_adc_sync_dmac_set_ctrl);

    R_DMAC_Disable(&g_adc_sync_dmac_clear_ctrl);
    R_DMAC_Close(&g_adc_sync_dmac_clear_ctrl);
    g_adc_sync_dmac_clear_err = R_DMAC_Open(&g_adc_sync_dmac_clear_ctrl, &g_adc_sync_dmac_clear_cfg);
    if (g_adc_sync_dmac_clear_err == FSP_SUCCESS) g_adc_sync_dmac_clear_err = R_DMAC_Enable(&g_adc_sync_dmac_clear_ctrl);

    after(msec(MARKER_DMAC_REARM_MS), adc_sync_dmac_rearm);
}

void adc_sync_marker_init(void) {
    g_adc_sync_elc_err = R_ELC_Open(&g_adc_sync_elc_ctrl, &g_adc_sync_elc_cfg);
    if (g_adc_sync_elc_err == FSP_SUCCESS) g_adc_sync_elc_err = R_ELC_Enable(&g_adc_sync_elc_ctrl);

    R_IOPORT_Open(&g_adc_sync_ioport_ctrl, &g_adc_sync_ioport_cfg);
    R_IOPORT_PinCfg(&g_adc_sync_ioport_ctrl, BSP_IO_PORT_01_PIN_12,
                     (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW);  // P112, plain GPIO

    g_adc_sync_dmac_set_err = R_DMAC_Open(&g_adc_sync_dmac_set_ctrl, &g_adc_sync_dmac_set_cfg);
    if (g_adc_sync_dmac_set_err == FSP_SUCCESS) g_adc_sync_dmac_set_err = R_DMAC_Enable(&g_adc_sync_dmac_set_ctrl);

    g_adc_sync_dmac_clear_err = R_DMAC_Open(&g_adc_sync_dmac_clear_ctrl, &g_adc_sync_dmac_clear_cfg);
    if (g_adc_sync_dmac_clear_err == FSP_SUCCESS) g_adc_sync_dmac_clear_err = R_DMAC_Enable(&g_adc_sync_dmac_clear_ctrl);

    // GPT3: open+enable+start here, once, same as the ELC links above —
    // harmless to leave running all the time (it only ever clears on
    // ACMPLP0's own ELC event, which never fires before dac8-cmp configures
    // and enables the comparator), no separate lifecycle tied to
    // adc_sync_start/stop.
    //
    // R_GPT_Enable() IS required here, confirmed wrong on the bench (same
    // class of miss as adc_sync_start()'s own R_ADC_ScanStart() comment
    // just below): R_GPT_Open() applies period/compare-match/mode, but
    // GTSSR/GTPSR/GTCSR (the external start/stop/clear SOURCE registers)
    // are a separate write, done only by R_GPT_Enable() (ra/fsp/src/r_gpt/
    // r_gpt.c, R_GPT_Enable(), ~line 363) — without it GTCSR keeps its
    // power-on-reset value (0, no clear source enabled) regardless of what
    // clear_source says in the extended cfg, so GPT3 just free-ran its
    // full period every time, never actually cleared by ACMPLP0's edge.
    // GPT1's own g_at_gpt_ext never needed this (its start/stop/clear
    // sources are all GPT_SOURCE_NONE — it free-runs on purpose), which is
    // exactly why copying its Open+Start pattern here missed this step.
    g_adc_sync_cmp_delay_err = R_GPT_Open(&g_adc_sync_cmp_delay_ctrl, &g_adc_sync_cmp_delay_cfg);
    if (g_adc_sync_cmp_delay_err == FSP_SUCCESS) g_adc_sync_cmp_delay_err = R_GPT_Enable(&g_adc_sync_cmp_delay_ctrl);
    if (g_adc_sync_cmp_delay_err == FSP_SUCCESS) {
        g_adc_sync_cmp_delay_open = true;
        g_adc_sync_cmp_delay_err  = R_GPT_Start(&g_adc_sync_cmp_delay_ctrl);
    }

    after(msec(MARKER_DMAC_REARM_MS), adc_sync_dmac_rearm);
}

// cmpdelay-us! — ( us -- ) retune GPT3's delay from ACMPLP0's ELC event to
// group A's ADC trigger, without a reflash — see ADC_CMP_DELAY_US_DEFAULT's
// own comment for the starting-guess math. Writes GTCCRA directly, same
// convention aoffset-us! already uses for GTCCRB (no -1: that adjustment is
// compare_match_value's own input convention at R_GPT_Open time, not what
// the raw register wants). Only takes effect once adc_sync_marker_init()
// has actually opened GPT3 — silently does nothing before that, same as
// every other register-poke CLI word in this file/analog_test.c.
// Upper bound (21us = 63 counts) is comfortably inside a single switching
// cycle's worth of margin, not a claim that values anywhere near it are
// good — tune against the scope.
void adc_sync_cli_cmpdelay_set(void) {   // ( us -- )
    Cell v = ret();
    if (v < 0 || v > 21) {
        print("cmpdelay-us!: 0-21us"); printCr();
        return;
    }
    if (!g_adc_sync_cmp_delay_open) return;
    R_GPT3->GTCCR[0] = (uint32_t) ((ADC_CMP_DELAY_HZ / 1000000U) * (uint32_t) v);
}

void adc_sync_cli_status(void) {
    // Live GPT1 and ADC0 peek, one level below everything else this word
    // already prints — ELC/DMAC/marker can all look perfectly healthy (and
    // have, on the bench, moments after a bad pstatus row) while telling
    // you nothing about whether GPT1 is actually still counting or the ADC
    // peripheral itself still thinks a hardware trigger is armed. GTCR.CST
    // ("Count Start") is cleared only by an explicit R_GPT_Stop() — nothing
    // in this file's own code calls that outside analog-test-stop/dac8-stop.
    // ADCSR.ADST is momentary (1 while a conversion is in flight, 0 the
    // rest of the ~64us cycle) so catching 0 here proves nothing by itself;
    // ADCSR.TRGE is the one that matters — R_ADC_ScanStart() sets it once
    // and nothing here should ever clear it while group-scan mode is live
    // (see adc_sync_start()'s own comment on what happens if it's never
    // set at all).
    uint32_t gtcr  = R_GPT1->GTCR;
    uint32_t gtcnt = R_GPT1->GTCNT;
    uint32_t adcsr = R_ADC0->ADCSR;
    print("GPT1 live: CST="); printDec0(gtcr & 1U);
    print("  GTCNT="); printDec0(gtcnt); print(" (analog_test.c's GPT1_PERIOD is 192 counts/cycle)"); printCr();
    print("ADC0 live: ADST="); printDec0((adcsr >> 15) & 1U);
    print("  TRGE="); printDec0((adcsr >> 9) & 1U);
    print("  ADCS="); printDec0((adcsr >> 13) & 3U); printCr();

    // GPT3 live peek — same reasoning as GPT1's above, one level below the
    // ELC/scope-marker state: CST proves it's actually counting (open+start
    // in adc_sync_marker_init() succeeded), GTCNT cycling between ~0 and
    // ADC_CMP_DELAY_COUNTS on repeated calls proves ACMPLP0's edge is
    // actually clearing it every carrier cycle, not just sitting parked.
    uint32_t gpt3_gtcr  = g_adc_sync_cmp_delay_open ? R_GPT3->GTCR  : 0U;
    uint32_t gpt3_gtcnt = g_adc_sync_cmp_delay_open ? R_GPT3->GTCNT : 0U;
    print("GPT3 live: open err="); printDec0((unsigned) g_adc_sync_cmp_delay_err);
    print("  CST="); printDec0(gpt3_gtcr & 1U);
    print("  GTCNT="); printDec0(gpt3_gtcnt);
    print(" (0 or small if ACMPLP0 isn't toggling / isn't clearing it yet)"); printCr();

    print("ELC: open err="); printDec0((unsigned) g_adc_sync_elc_err);
    print("  links: ADC0<-GPT3-compare-A(comparator-edge+delay)  ADC0_B<-GPT1-compare-B(half-step offset)  ELC_GPTA<-ACMPLP0_INT"); printCr();
    print("  group A delay from ACMPLP0's ELC event (cmpdelay-us! to retune): ");
    printDec0(R_GPT3->GTCCR[0] / 3U); print("us ("); printDec0(R_GPT3->GTCCR[0]); print(" counts)"); printCr();

    print("Scope marker (P112, plain GPIO via DMAC, no GPT output):"); printCr();
    print("  DMAC1 (set on ADC0 scan-end):   open err="); printDec0((unsigned) g_adc_sync_dmac_set_err); printCr();
    print("  DMAC2 (clear on GPT1 overflow): open err="); printDec0((unsigned) g_adc_sync_dmac_clear_err); printCr();
    print("P112 reads HIGH once group A's conversion is valid, LOW again once the"); printCr();
    print("next carrier cycle starts re-acquiring. Only moves once dac8-cmp's"); printCr();
    print("carrier (GPT1) is running and adc_sync_start() is active."); printCr();

    // Live ELC register peek — confirms the three links actually landed in
    // hardware as configured, and that the master enable (ELCR.ELCON, set
    // by R_ELC_Enable()) is on. ELSR[n].ELS holds the source event number
    // (same numeric value as the elc_event_t enum): ADC0 should now read
    // 0x06F (GPT3 compare-match A), ADC0_B still 0x060 (GPT1 compare-match
    // B, unchanged), and the new GPT_A slot 0x02F (ACMPLP0_INT).
    uint32_t elcr      = R_ELC->ELCR;
    uint32_t els_adc0   = R_ELC->ELSR[ELC_PERIPHERAL_ADC0].HA & 0x1FFU;
    uint32_t els_adc0b  = R_ELC->ELSR[ELC_PERIPHERAL_ADC0_B].HA & 0x1FFU;
    uint32_t els_gpta   = R_ELC->ELSR[ELC_PERIPHERAL_GPT_A].HA & 0x1FFU;
    print("ELC live: ELCON="); printDec0((elcr >> 7) & 1U);
    print("  ADC0<-0x"); printHex2(els_adc0);
    print("  ADC0_B<-0x"); printHex2(els_adc0b);
    print("  GPTA<-0x"); printHex2(els_gpta);
    print("  (want 6F/60/2F)"); printCr();

    // Live P112 PFS peek — confirms it's genuinely plain GPIO (PMR=0), not
    // still claimed by a GPT peripheral from an earlier build.
    uint32_t pfs = R_PFS->PORT[1].PIN[12].PmnPFS;
    print("P112 PFS: PMR="); printDec0((pfs >> 16) & 1U);
    print("  PDR(dir, 1=out)="); printDec0((pfs >> 2) & 1U);
    print("  PODR(out latch)="); printDec0(pfs & 1U);
    print("  PIDR(pad level)="); printDec0((pfs >> 1) & 1U);
    printCr();

    // Live DMAC register peek, same idea as the GPT register dumps earlier
    // in this file's history: DMCNT.DTE confirms the channel is actually
    // enabled; DMSTS.ACT is a momentary "mid-transfer right now" flag;
    // DMCRA.DMCRAL is the live per-activation countdown (reloads to 1 —
    // our configured length — after each transfer, so catching it at 0 or
    // seeing it differ between the two prints proves real activity, not
    // just a parked, never-triggered channel).
    uint32_t dmcnt1 = R_DMAC1->DMCNT, dmsts1 = R_DMAC1->DMSTS, dmcra1 = R_DMAC1->DMCRA;
    uint32_t dmcnt2 = R_DMAC2->DMCNT, dmsts2 = R_DMAC2->DMSTS, dmcra2 = R_DMAC2->DMCRA;
    print("DMAC1 (set):   DTE="); printDec0(dmcnt1 & 1U);
    print(" ACT="); printDec0((dmsts1 >> 7) & 1U);
    print(" DTIF="); printDec0((dmsts1 >> 4) & 1U);
    print(" DMCRAL="); printDec0(dmcra1 & 0xFFFFU);
    printCr();
    print("DMAC2 (clear): DTE="); printDec0(dmcnt2 & 1U);
    print(" ACT="); printDec0((dmsts2 >> 7) & 1U);
    print(" DTIF="); printDec0((dmsts2 >> 4) & 1U);
    print(" DMCRAL="); printDec0(dmcra2 & 0xFFFFU);
    printCr();
}

// adc_sync_marker_ready() — true when P112 is currently high, i.e. the
// group A result registers (Vsense/Vdrain) hold the value from the most
// recent completed scan, not one still being overwritten by the next
// ~64us DMA cycle. A caller that samples Vsense/Vdrain on its own timer
// (wire-sweep, any future bench loop) has no way to know where its read
// lands relative to that cycle otherwise — the marker is the one thing
// both sides share. Plain PIDR read, same register pins.c reads for
// `pin@`; no peripheral call needed, it's just the pad's current level.
bool adc_sync_marker_ready(void) {
    return ((R_PFS->PORT[1].PIN[12].PmnPFS >> 1) & 1U) != 0U;
}

// adc_sync_wait_ready() — same gate as adc_sync_marker_ready(), but for a
// plain synchronous reader (power_sense.c's pstatus/vsense@/vdrain@, any
// single-shot CLI read) that returns a value directly and so can't
// reschedule itself through TEA the way analog_test.c's wire_sweep_sample
// does. Busy-waits up to ~200us (comfortably more than one ~64us cycle's
// worth of margin) for the marker to go high, then returns either way —
// bounded the same way dma-watch's own tight loop is, not an unbounded
// spin, and short enough not to be the "long-running action" the
// cooperative-scheduler rule (TimbreOS/CLAUDE.md) is actually about. If
// adc_sync_start() was never called (continuous-scan mode, no dac8-cmp
// running), the marker just never toggles and the pin reads whatever
// adc_sync_marker_init() last set it to — the timeout keeps that case a
// cheap no-op instead of a silent hang.
void adc_sync_wait_ready(void) {
    uint32_t start = (uint32_t) sysTicks();
    while (!adc_sync_marker_ready()) {
        if (((uint32_t) sysTicks() - start) > 48U * 200U) return;   // ~200us at 48MHz
    }
}

// CLI word: dma-watch — tight ~300us busy-loop sample of both DMAC
// channels' DMSTS.ACT (momentary "transfer in progress") and DMCRA.DMCRAL
// (live per-activation countdown, reloads after each transfer), same
// technique that caught GPT3 free-running earlier in this file's history.
// If dac8-cmp's carrier is genuinely retriggering these every ~64us, a
// 300us window should span several firings — ACT seen 1 and/or DMCRAL
// changing proves real activity; both pinned/unchanged means the channel
// is parked, never actually triggered by ADC0_SCAN_END / GPT1 overflow.
void adc_sync_cli_dma_watch(void) {
    uint32_t start = (uint32_t) sysTicks();
    uint32_t budget_cycles = 48U * 300U;   // ~300us at 48MHz
    uint32_t act1_seen = 0U, act2_seen = 0U;
    uint32_t cra1_min = 0xFFFFFFFFU, cra1_max = 0U, cra2_min = 0xFFFFFFFFU, cra2_max = 0U;
    uint32_t samples = 0U;

    while (((uint32_t) sysTicks() - start) < budget_cycles) {
        uint32_t dmsts1 = R_DMAC1->DMSTS, dmcra1 = R_DMAC1->DMCRA & 0xFFFFU;
        uint32_t dmsts2 = R_DMAC2->DMSTS, dmcra2 = R_DMAC2->DMCRA & 0xFFFFU;
        samples++;
        if ((dmsts1 >> 7) & 1U) act1_seen = 1U;
        if ((dmsts2 >> 7) & 1U) act2_seen = 1U;
        if (dmcra1 < cra1_min) cra1_min = dmcra1;
        if (dmcra1 > cra1_max) cra1_max = dmcra1;
        if (dmcra2 < cra2_min) cra2_min = dmcra2;
        if (dmcra2 > cra2_max) cra2_max = dmcra2;
    }

    print("dma-watch: "); printDec0(samples); print(" samples over ~300us"); printCr();
    print("  DMAC1 (set):   ACT ever 1 = "); printDec0(act1_seen);
    print("   DMCRAL range "); printDec0(cra1_min); print(".."); printDec0(cra1_max); printCr();
    print("  DMAC2 (clear): ACT ever 1 = "); printDec0(act2_seen);
    print("   DMCRAL range "); printDec0(cra2_min); print(".."); printDec0(cra2_max); printCr();
}
