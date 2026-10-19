// Board/analog_test.h — bench validation for the DAC12/DAC8/ACMPLP/OPAMP0
// path before any of it is wired to the breadboard muscle-wire circuit.
//
// Two independent checks, run separately so a failure says which piece is
// wrong:
//
//   dac12-ramp   GPT1 overflow -> ELC -> DMAC writes a lookup table into
//                DAC12 (P014) at a fixed rate. Scope P014 directly; this
//                is the only piece with an external, already-proven pin.
//
//   dac8-cmp     Same GPT1/ELC/DMAC chain, but the destination is DAC8 ch0
//                (internal only, no pin) and DAC8's value becomes ACMPLP0's
//                reference (IVREF0). The other input (IVCMP0) is CMPIN0,
//                P100 — apply a known external test voltage there. ACMPLP0's
//                output is VCOUT, P110 (NOT P109 — P109 only carries CLKOUT
//                at that same PSEL value; P110 is the one with VCOUT). As
//                DAC8 ramps through the CMPIN0 voltage, VCOUT should flip —
//                that one transition is the proof DAC8's internal route
//                into the comparator is real, since DAC8's own voltage can
//                never be probed directly.
//
// Uses GPT1. GPT0 is TEA's own system tick (Board/clocks.c), GPT2 is
// delta_timer, GPT7 is the muscle frame timer (frame_timer.c) — all three
// claimed elsewhere and confirmed not to touch. An earlier version of this
// file used GPT0 on an unverified assumption that it was free; it wasn't,
// and reconfiguring it froze TEA's own time-event list on the bench.
//
// Register-level details confirmed against the RA4M1 Hardware User's Manual
// (Docs/r01uh0887ej0110-ra4m1.pdf): COMPSEL0/COMPSEL1 (39.2.4/39.2.5),
// the ACMPLP setup procedure (Table 39.3), P110's VCOUT pin function and
// PSEL=01001b/IOPORT_PERIPHERAL_CLKOUT_COMP_RTC (Table 19.7), and that
// analog inputs like CMPIN0 need PMR=0 with only ASEL set (19.5.4) — not
// IOPORT_CFG_PERIPHERAL_PIN, which an earlier draft of this file wrongly
// added. Still worth confirming empirically: run `dac8-cmp` with a bench
// supply on CMPIN0 and watch P110 before trusting the rest.

#ifndef BOARD_ANALOG_TEST_H
#define BOARD_ANALOG_TEST_H

void analog_test_cli_gpt1_only(void);   // CLI word: gpt1-only — isolation test, no DAC/DMAC/ACMPLP at all
void analog_test_cli_dmac_only(void);   // CLI word: dmac-only — gpt1-only + active DMAC, DAC12 left unclocked
void analog_test_cli_dac12_ramp(void);  // CLI word: dac12-ramp
void analog_test_cli_dac8_cmp(void);    // CLI word: dac8-cmp
void analog_test_cli_stop(void);        // CLI word: analog-test-stop
void analog_test_cli_status(void);      // CLI word: dac-status — MSTP/register state for DAC12, DAC8, ACMPLP, GPT1

#endif
