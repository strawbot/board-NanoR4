// Board/frame_timer.c — GPT7 as the muscle frame timer.
//
// GPT7 is a 16-bit timer. PCLKD (48 MHz) / 16 = 3 MHz, 0.33 us per count, and one
// 20 ms frame is 60000 counts, which fits. It free-runs and wraps every frame.
// Three interrupt sources drive muscle_sr (Robot/limbs/muscle/muscle_sr.c):
//
//   counter overflow  frame start       -> muscle_sr_isr_frame()
//   compare match A   next window edge  -> muscle_sr_isr_edge()
//   compare match B   sample point      -> muscle_sr_isr_sample()
//
// FSP opens and starts the timer (its register sequence is the known-good one),
// but the vector table points straight at the short handlers below instead of
// FSP's callback ISRs: those cost microseconds this path cannot spare. The
// handlers are in ra_gen/vector_data.c, slots 14-16, with the matching
// event links (edit by hand; regenerating from RASC removes them, as it does the
// USB vector change noted there).
//
// Priorities: the frame and edge interrupts are 1, the sample interrupt 3, and
// everything else in this project runs at 12, so a window edge is never held up
// by the UART, USB or the scheduler tick. An interrupt-masked region (the TEA
// safe() blocks) can still delay one for its few microseconds.

#include "hal_data.h"
#include "r_gpt.h"
#include "vector_data.h"
#include "muscle_sr.h"
#include "frame_timer.h"

#define FT_HZ        3000000U
#define FT_FRAME     (FT_HZ / 1000U * MUSCLE_SR_FRAME_MS)     // 60000 counts
#define FT_IPL_EDGE  1U
#define FT_IPL_SAMPLE 3U

static gpt_instance_ctrl_t g_ft_ctrl;

static const gpt_extended_cfg_t g_ft_ext = {
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
    .capture_a_ipl = FT_IPL_EDGE,
    .capture_b_ipl = FT_IPL_SAMPLE,
    .capture_a_irq = VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_A,
    .capture_b_irq = VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_B,
    // Compare match enabled on A and B (a value of 1 puts GTCCR at 0; muscle_sr
    // rewrites them before it relies on them).
    .compare_match_value  = { 1U, 1U },
    .compare_match_status = 0x03U,
    .p_pwm_cfg = NULL,
    .gtior_setting.gtior = 0U,
    .gtioca_polarity = GPT_GTIOC_POLARITY_NORMAL,
    .gtiocb_polarity = GPT_GTIOC_POLARITY_NORMAL,
};

static const timer_cfg_t g_ft_cfg = {
    .mode              = TIMER_MODE_PERIODIC,
    .period_counts     = FT_FRAME,
    .duty_cycle_counts = 0U,
    .source_div        = TIMER_SOURCE_DIV_16,
    .channel           = 7U,
    .p_callback        = NULL,
    .p_context         = NULL,
    .p_extend          = &g_ft_ext,
    .cycle_end_ipl     = FT_IPL_EDGE,
    .cycle_end_irq     = VECTOR_NUMBER_GPT7_COUNTER_OVERFLOW,
};

// ── the hooks muscle_sr calls ─────────────────────────────────

static void ft_start(void) { R_GPT_Start(&g_ft_ctrl); }

static void ft_stop(void) {
    R_BSP_IrqDisable(VECTOR_NUMBER_GPT7_COUNTER_OVERFLOW);
    R_BSP_IrqDisable(VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_A);
    R_BSP_IrqDisable(VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_B);
    R_GPT_Stop(&g_ft_ctrl);
}

static uint32_t ft_count(void) { return R_GPT7->GTCNT; }

// Compare registers are written without a buffer (FSP clears the buffer enables
// when compare match is in use), and the match is GTCNT == GTCCR.
static void ft_arm_edge(uint32_t counts)   { R_GPT7->GTCCR[0] = counts; }
static void ft_arm_sample(uint32_t counts) { R_GPT7->GTCCR[1] = counts; }

static const muscle_sr_timer_t g_ft = {
    .hz           = FT_HZ,
    .frame_counts = FT_FRAME,
    .start        = ft_start,
    .stop         = ft_stop,
    .count        = ft_count,
    .arm_edge     = ft_arm_edge,
    .arm_sample   = ft_arm_sample,
};

void frame_timer_bind(void) {
    R_GPT_Open(&g_ft_ctrl, &g_ft_cfg);
    muscle_sr_bind_timer(&g_ft);
}

// ── the interrupt handlers (vector_data.c slots 14-16) ────────

void frame_timer_frame_isr(void) {
    R_BSP_IrqStatusClear(VECTOR_NUMBER_GPT7_COUNTER_OVERFLOW);
    muscle_sr_isr_frame();
}

void frame_timer_edge_isr(void) {
    R_BSP_IrqStatusClear(VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_A);
    muscle_sr_isr_edge();
}

void frame_timer_sample_isr(void) {
    R_BSP_IrqStatusClear(VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_B);
    muscle_sr_isr_sample();
}
