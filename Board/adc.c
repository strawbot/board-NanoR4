// adc.c — ADC0 unit 0, AN00 (P000), 14-bit continuous scan.
//
// Full-scale is treated as 5000 mV: the physical 0–5 V signal is assumed to
// reach P000 within the MCU's analog input range via external scaling, and the
// 5 V reference voltage is used for the raw→mV conversion.

#include <stdint.h>
#include "hal_data.h"
#include "tea.h"
#include "cli.h"
#include "printers.h"

// 14-bit full scale = 16383 counts; reference treated as 5000 mV.
#define ADC_VREF_MV    5000UL
#define ADC_FULL_SCALE 16383UL

void adc_init(void) {
    R_ADC_Open(&g_adc0_ctrl, &g_adc0_cfg);
    R_ADC_ScanCfg(&g_adc0_ctrl, &g_adc0_channel_cfg);
    R_ADC_ScanStart(&g_adc0_ctrl);
}

// Returns the most recent AN00 reading in millivolts (0–5000).
// Continuous scan mode — no blocking wait needed.
uint32_t adc_read_mv(void) {
    uint16_t raw = 0;
    R_ADC_Read(&g_adc0_ctrl, ADC_CHANNEL_0, &raw);
    return (uint32_t)raw * ADC_VREF_MV / ADC_FULL_SCALE;
}

// CLI word: pushes the current AN00 reading in mV onto the data stack.
void cli_adc_read(void) {
    lit((Cell)adc_read_mv());
}
