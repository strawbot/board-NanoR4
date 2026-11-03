// Board/analog_test.h — bench validation for the DAC12/DAC8/ACMPLP/OPAMP0
// path before any of it is wired to the breadboard muscle-wire circuit.
//
// Two independent checks, run separately so a failure says which piece is
// wrong:
//
//   dac12-ramp   GPT1 overflow -> ELC -> DMAC writes a lookup table into
//                DAC12 (P014) at a fixed rate. Scope P014 directly; this
//                is the only piece with an external, already-proven pin.
//                The DMA's TRANSFER_MODE_REPEAT is not actually infinite:
//                its repeat counter (DMCRB) exhausts after 65536 cycles of
//                the 64-sample table, ~268s (~4.5 min), at which point the
//                DMAC stops itself (confirmed on the bench: dac8-cmp's
//                carrier went silent after a few minutes of otherwise-
//                untouched operation). Both this and dac8-cmp now
//                periodically Close+Open+Enable the DMAC well inside that
//                window (analog_test_dmac_rearm) so it never actually runs
//                out during normal bench use.
//
//   dac8-cmp     Same GPT1/ELC/DMAC chain, but the destination is DAC8 ch0
//                (internal only, no pin) and DAC8's value becomes ACMPLP0's
//                reference (IVREF0). The other input (IVCMP0) is CMPIN0,
//                P100. ACMPLP0's output is VCOUT, P110 (NOT P109 — P109
//                only carries CLKOUT at that same PSEL value; P110 is the
//                one with VCOUT). Also enables OPAMP0 itself (same as
//                opamp0-test) — CMPIN0 is meant to be fed by AMP0O in the
//                real trigger path, so this test owns that rather than
//                leaving "remember to run opamp0-test first" as a separate
//                step; only the external jumpers stay the user's job. DAC8's
//                carrier is a band (DAC8_CARRIER_MIN/MAX in analog_test.c,
//                currently ~102-5088mV) rather than sweeping true edge to
//                edge: any CMPIN0 value inside the carrier's own range gets
//                crossed every cycle regardless of what it is, so a CMPIN0
//                sitting right at a full-scale carrier's boundary (e.g. 0V)
//                produces a steady trickle of boundary-grazing edges
//                instead of true silence. With the carrier narrowed, a
//                CMPIN0 value outside the band on either side settles to a
//                steady, non-toggling state (the cap-coupled RC stage
//                downstream only responds to edges, not DC level), and a
//                value inside the band produces real toggling — confirmed
//                on the bench with dac12!: 0mV silent, ~2500mV actively
//                pulsing the gate (at the original, tighter ~1020-4075mV
//                band tuned for that test). That band was later widened to
//                ~102-5088mV for mapping the wire's characteristic at a
//                higher error-amp gain (R2=470k), where AMP0O's excursion
//                across the wire's full onset-to-saturation range turned
//                out to be wider than the tighter band could represent —
//                it was cutting the sweep off (comparator settling to a
//                steady "off" state) before the wire's real saturation
//                plateau ever showed up. If trigger-pulse testing needs
//                the tighter band's cleaner silence margin again, only
//                DAC8_CARRIER_MIN/MAX need to change back.
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

void analog_test_init(void);            // names this file's self-rescheduling actions for mstats; call once from main

void analog_test_cli_gpt1_only(void);   // CLI word: gpt1-only — isolation test, no DAC/DMAC/ACMPLP at all
void analog_test_cli_dmac_only(void);   // CLI word: dmac-only — gpt1-only + active DMAC, DAC12 left unclocked
void analog_test_cli_dac12_ramp(void);  // CLI word: dac12-ramp
void analog_test_cli_dac12_set(void);   // CLI word: dac12! ( mv -- ) hold DAC12 at a fixed voltage, no ramp
void analog_test_cli_dac8_cmp(void);    // CLI word: dac8-cmp
void analog_test_cli_opamp0(void);      // CLI word: opamp0-test
void analog_test_cli_opamp0_sweep(void); // CLI word: opamp0-sweep
void analog_test_cli_wire_sweep(void);   // CLI word: wire-sweep — dac8-cmp must already be running; steps dac12 0-1000mV, prints rwire/vsense/vdrain stats per setpoint, then an onset/mid/full summary via wirecal_analyze()
void analog_test_cli_wstep_set(void);      // CLI word: wstep! ( mv -- ) wire-sweep's setpoint increment, 1-1500mV (default 50)
void analog_test_cli_wsettle_set(void);    // CLI word: wsettle! ( ms -- ) wire-sweep's per-step settle time before the first sample
void analog_test_cli_wsample_ms_set(void); // CLI word: wsample-ms! ( ms -- ) wire-sweep's spacing between samples within a step
void analog_test_cli_wsamples_set(void);   // CLI word: wsamples! ( n -- ) wire-sweep's sample count per step
void analog_test_cli_stop(void);        // CLI word: analog-test-stop
void analog_test_cli_dac8_stop(void);   // CLI word: dac8-stop — stops just the carrier/comparator, leaves OPAMP0 running
void analog_test_cli_cmpfir_set(void);  // CLI word: cmpfir! ( n -- ) ACMPLP0 output resample: 0 bypass, 1 PCLKB, 2 PCLKB/8, 3 PCLKB/32
void analog_test_cli_aoffset_set(void);  // CLI word: aoffset-us! ( us -- ) group A's (Vsense/Vdrain) ADC trigger offset from GPT1 overflow, 0-31
void analog_test_cli_status(void);      // CLI word: dac-status — MSTP/register state for DAC12, DAC8, ACMPLP, OPAMP0, GPT1

#endif
