// Board/adc_sync.h — one-time boot setup for the ELC links the hardware-
// triggered ADC path (Board/adc.c's adc_sync_start/stop) and its scope-
// verification pulse depend on. See adc_sync.c for the full design.

#ifndef BOARD_ADC_SYNC_H
#define BOARD_ADC_SYNC_H

void adc_sync_marker_init(void);  // call once at boot, after adc_init()
void adc_sync_cli_status(void);   // CLI word: trig-status
void adc_sync_cli_dma_watch(void); // CLI word: dma-watch — ~300us tight-loop sample of both DMAC channels' ACT/DMCRAL

#endif
