// Board/adc_sync.c — see adc_sync.h.
//
// Two independent, hardware-only (zero CPU/scheduler jitter) ELC links,
// both riding on ELC_EVENT_GPT1_COUNTER_OVERFLOW — the same event that
// already drives the carrier's own DMAC step (Board/analog_test.c) — fanned
// out to a second destination, which ELC supports for free:
//
//   GPT1 overflow      -> ADC0 group A trigger (ELC_AD00)  -- Vsense, Vdrain
//   GPT1 compare-match B (half-step offset, Board/analog_test.c's
//     g_at_gpt_ext.compare_match_value[1]) -> ADC0 group B trigger (ELC_AD01)
//                                                          -- legacy + VIN
//
// Group B does NOT share group A's overflow event — confirmed wrong on the
// bench (VIN stuck at 0 forever). Per the manual (35.3.2): "if the
// conversion processes in group A and B occur at the same time, those
// conversions cannot be controlled separately" — with one shared event,
// every trigger was simultaneous, and group B lost every single time, not
// occasionally. Giving it GPT1's own compare-match B, offset half a carrier
// step from the overflow, gives both groups the same once-per-64us cadence
// without ever colliding.
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
#include "r_ioport.h"
#include "adc_sync.h"
#include "board_cli.h"
#include "cli.h"
#include "printers.h"
#include "tea.h"   // sysTicks()

static elc_instance_ctrl_t g_adc_sync_elc_ctrl;

static const elc_cfg_t g_adc_sync_elc_cfg = {
    .link = {
        [ELC_PERIPHERAL_ADC0]   = ELC_EVENT_GPT1_COUNTER_OVERFLOW,
        [ELC_PERIPHERAL_ADC0_B] = ELC_EVENT_GPT1_CAPTURE_COMPARE_B,
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

    after(msec(MARKER_DMAC_REARM_MS), adc_sync_dmac_rearm);
}

void adc_sync_cli_status(void) {
    print("ELC: open err="); printDec0((unsigned) g_adc_sync_elc_err);
    print("  links: ADC0<-GPT1-overflow  ADC0_B<-GPT1-compare-B(half-step offset)"); printCr();

    print("Scope marker (P112, plain GPIO via DMAC, no GPT output):"); printCr();
    print("  DMAC1 (set on ADC0 scan-end):   open err="); printDec0((unsigned) g_adc_sync_dmac_set_err); printCr();
    print("  DMAC2 (clear on GPT1 overflow): open err="); printDec0((unsigned) g_adc_sync_dmac_clear_err); printCr();
    print("P112 reads HIGH once group A's conversion is valid, LOW again once the"); printCr();
    print("next carrier cycle starts re-acquiring. Only moves once dac8-cmp's"); printCr();
    print("carrier (GPT1) is running and adc_sync_start() is active."); printCr();

    // Live ELC register peek — confirms the two ADC-trigger links actually
    // landed in hardware as configured, and that the master enable
    // (ELCR.ELCON, set by R_ELC_Enable()) is on. ELSR[n].ELS holds the
    // source event number (same numeric value as the elc_event_t enum), so
    // these should read 0x065 (GPT1 overflow) and 0x060 (GPT1 compare-match
    // B) respectively if the config in this file actually took.
    uint32_t elcr      = R_ELC->ELCR;
    uint32_t els_adc0   = R_ELC->ELSR[ELC_PERIPHERAL_ADC0].HA & 0x1FFU;
    uint32_t els_adc0b  = R_ELC->ELSR[ELC_PERIPHERAL_ADC0_B].HA & 0x1FFU;
    print("ELC live: ELCON="); printDec0((elcr >> 7) & 1U);
    print("  ADC0<-0x"); printHex2(els_adc0);
    print("  ADC0_B<-0x"); printHex2(els_adc0b);
    print("  (want 65/60)"); printCr();

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
