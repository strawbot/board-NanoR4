// Board/adc_sync.h — one-time boot setup for the ELC links the hardware-
// triggered ADC path (Board/adc.c's adc_sync_start/stop) and its scope-
// verification pulse depend on. See adc_sync.c for the full design.

#ifndef BOARD_ADC_SYNC_H
#define BOARD_ADC_SYNC_H

#include <stdbool.h>

void adc_sync_marker_init(void);  // call once at boot, after adc_init()
void adc_sync_cli_status(void);   // CLI word: trig-status
void adc_sync_cli_dma_watch(void); // CLI word: dma-watch — ~300us tight-loop sample of both DMAC channels' ACT/DMCRAL
void adc_sync_cli_cmpdelay_set(void); // CLI word: cmpdelay-us! — ( us -- ) retune GPT3's comparator-edge-to-ADC-trigger delay (group A), 0-21us

// true once ADC0_SCAN_END has set P112 for the group now sitting in the
// result registers (i.e. not mid-reacquire); see adc_sync_marker_ready()
// in adc_sync.c for why a polling loop should check this before reading
// Vsense/Vdrain rather than trusting its own timer interval alone.
bool adc_sync_marker_ready(void);

// Bounded busy-wait for adc_sync_marker_ready() — for a plain synchronous
// reader (pstatus, vsense@, ...) that can't reschedule itself through TEA
// the way wire-sweep's own action does. See adc_sync_wait_ready() in
// adc_sync.c for the timeout. A no-op (returns immediately) if dac8-cmp/
// adc_sync_start() isn't running, rather than hanging.
void adc_sync_wait_ready(void);

#endif
