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
// added. Confirmed empirically: `dac8-cmp` with a bench supply on CMPIN0
// produced a clean VCOUT transition, and feeding VCOUT through an external
// cap-coupled Schmitt stage held up to 98.5% duty before flattening, as
// designed by the gate-protector network.
//
//   opamp0-test  Turns on OPAMP0 (AMP0+ = P000, AMP0- = P001, AMP0O = P002)
//                as a plain 3-pin differential amp — no DMA/ramp involved,
//                you feed it real test voltages externally. This is the
//                only one of the four on-chip OPAMP units with all three
//                pins exposed on this board (OPAMP1/2 only expose their
//                output, P003/P004; OPAMP3's pins aren't exposed at all).
//                Its three pins double as AN000/AN001/AN002 on the old
//                hex-board ISNS_B/ISNS_C/VIN_S scan (Board/adc.c) — moot
//                now that the hex board is abandoned, but it's why this
//                is the one unit worth checking against Table 35.12
//                before reusing. The CMSIS header declares an OS/MS/PS
//                input/output selector per unit (R_OPAMP->AMP[n]); the
//                manual's RA4M1-specific text (ch.38, Figure 38.1, Table
//                38.1) never mentions it and shows only the three external
//                pins, so — like DACR's bogus "DAE" bit and the bogus
//                IOPORT_PERIPHERAL_ACMPHS constant found earlier in this
//                file — treat it as not implemented on this part and
//                don't write to it. Stabilization wait time (activating
//                AMPE0+IREFE together, low-power mode) is 650 us min per
//                HWM Table 48.68 (Tstd3); msec(1) covers it the same way
//                the ACMPLP's 100 us Tcmp was covered.
//
//   opamp0-sweep Jumper DAC12's pin (P014) to AMP0+ (P000) externally —
//                there is no internal route from DAC12 or DAC8 into
//                OPAMP0's inputs; Figure 38.1's "MUX" blocks are only on
//                the activation-trigger path (AGT0/AGT1/A-D-end), not the
//                analog signal path, so the R_OPAMP->AMP[n].OS/MS/PS
//                registers the CMSIS header declares are, like the OS/MS/
//                PS-adjacent AMPHS mixup found earlier, not a real feature
//                on this part. Steps DAC12 through 0-4095 (static writes,
//                GPT1/DMAC freed first so they can't fight the register),
//                settles 1 ms per step (covers OPAMP0's own Tset1, 750 us
//                max in low-power mode per Table 48.68), and prints the
//                setpoint against the measured AMP0O (read back over the
//                ADC's existing AN002 scan channel, no extra wiring) —
//                paste the output into a spreadsheet to plot the transfer
//                curve.

#ifndef BOARD_ANALOG_TEST_H
#define BOARD_ANALOG_TEST_H

void analog_test_cli_gpt1_only(void);   // CLI word: gpt1-only — isolation test, no DAC/DMAC/ACMPLP at all
void analog_test_cli_dmac_only(void);   // CLI word: dmac-only — gpt1-only + active DMAC, DAC12 left unclocked
void analog_test_cli_dac12_ramp(void);  // CLI word: dac12-ramp
void analog_test_cli_dac12_set(void);   // CLI word: dac12! ( mv -- ) hold DAC12 at a fixed voltage, no ramp
void analog_test_cli_dac8_cmp(void);    // CLI word: dac8-cmp
void analog_test_cli_opamp0(void);      // CLI word: opamp0-test
void analog_test_cli_opamp0_sweep(void); // CLI word: opamp0-sweep
void analog_test_cli_stop(void);        // CLI word: analog-test-stop
void analog_test_cli_status(void);      // CLI word: dac-status — MSTP/register state for DAC12, DAC8, ACMPLP, OPAMP0, GPT1

#endif
