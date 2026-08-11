// adc.c — ADC0 unit 0, 14-bit continuous scan.
//
// Instance data is defined locally (not RASC-generated) because this
// module needs continuous-scan mode with a specific channel mask, and
// nothing in this project's RASC configuration produces that. A g_adc0_*
// stack now DOES exist in ra_gen/hal_data.c (default single-scan, no
// channels enabled, added while wiring up IIC1) — that's a separate,
// unopened, effectively-dead instance; ours is named g_board_adc_* to
// avoid colliding with it, and is the one actually driving ADC0 unit 0.
//
//   AN000 (P000) — gate-voltage monitor (original analog circuit sense point).
//   AN001 (P001) — muscle-wire 1 Ω sense resistor (PWM digital circuit).
//
// Full-scale is 5000 mV / 16383 counts (14-bit, AVCC0 reference).
// With a 1 Ω sense resistor: V_sense [mV] == I_sense [mA].

#include "r_adc.h"          // defines adc_instance_ctrl_t, adc_cfg_t, adc_channel_cfg_t
#include "hal_data.h"       // bsp_api.h, FSP_INVALID_VECTOR, BSP_IRQ_DISABLED
#include "tea.h"
#include "cli.h"
#include "printers.h"

#define ADC_VREF_MV    5000UL
#define ADC_FULL_SCALE 16383UL   // 2^14 - 1

// ── ADC instance (not generated; defined locally) ────────────────────────────

static adc_instance_ctrl_t g_board_adc_ctrl;   // DO NOT pre-initialize (FSP requirement)

static const adc_extended_cfg_t g_board_adc_cfg_extend = {
    .add_average_count   = ADC_ADD_OFF,
    .clearing            = ADC_CLEAR_AFTER_READ_ON,
    .trigger_group_b     = ADC_START_SOURCE_DISABLED,  // group mode unused (ADC_MODE_CONTINUOUS_SCAN below)
    .double_trigger_mode = ADC_DOUBLE_TRIGGER_DISABLED,
    .adc_vref_control    = ADC_VREF_CONTROL_AVCC0_AVSS0,
    .enable_adbuf        = 0U,
    .window_a_irq        = FSP_INVALID_VECTOR,
    .window_b_irq        = FSP_INVALID_VECTOR,
    .window_a_ipl        = BSP_IRQ_DISABLED,
    .window_b_ipl        = BSP_IRQ_DISABLED,
};

static const adc_cfg_t g_board_adc_cfg = {
    .unit           = 0U,
    .mode           = ADC_MODE_CONTINUOUS_SCAN,
    .resolution     = ADC_RESOLUTION_14_BIT,
    .alignment      = ADC_ALIGNMENT_RIGHT,
    .trigger        = ADC_TRIGGER_SOFTWARE,
    .scan_end_irq   = FSP_INVALID_VECTOR,
    .scan_end_b_irq = FSP_INVALID_VECTOR,
    .scan_end_ipl   = BSP_IRQ_DISABLED,
    .scan_end_b_ipl = BSP_IRQ_DISABLED,
    .p_callback     = NULL,
    .p_context      = NULL,
    .p_extend       = &g_board_adc_cfg_extend,
};

// Scan AN000 (P000) and AN001 (P001) together.
static const adc_channel_cfg_t g_board_adc_channel_cfg = {
    .scan_mask          = (1U << 0) | (1U << 1),  // AN000=P000, AN001=P001
    .scan_mask_group_b  = 0U,
    .add_mask           = 0U,
    .p_window_cfg       = NULL,
    .priority_group_a   = ADC_GROUP_A_PRIORITY_OFF,
    .sample_hold_mask   = 0U,   // 0 = no channel uses sample_hold_states below;
    .sample_hold_states = 24U,  // dead field as configured — reviewed for the 48MHz clock bump, nothing to change since it isn't applied to any channel.
};

// ── Init / read API ──────────────────────────────────────────────────────────

void adc_init(void) {
    R_ADC_Open(&g_board_adc_ctrl, &g_board_adc_cfg);
    R_ADC_ScanCfg(&g_board_adc_ctrl, &g_board_adc_channel_cfg);
    R_ADC_ScanStart(&g_board_adc_ctrl);
}

// AN000 (P000) reading in millivolts (0–5000).
uint32_t adc_read_mv(void) {
    uint16_t raw = 0;
    R_ADC_Read(&g_board_adc_ctrl, ADC_CHANNEL_0, &raw);
    return (uint32_t)raw * ADC_VREF_MV / ADC_FULL_SCALE;
}

// AN001 (P001) reading in milliamps.
// 1 Ω sense resistor → V_sense [mV] = I_sense [mA].
float adc_read_sense_ma(void) {
    uint16_t raw = 0;
    R_ADC_Read(&g_board_adc_ctrl, ADC_CHANNEL_1, &raw);
    return (float)raw * (float)ADC_VREF_MV / (float)ADC_FULL_SCALE;
}

// CLI word: push current AN000 reading in mV onto the data stack.
void cli_adc_read(void) {
    lit((Cell)adc_read_mv());
}
