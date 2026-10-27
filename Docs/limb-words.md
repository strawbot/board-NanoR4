# Limb words (PWM limb model)

The CLI words that drive and calibrate the limbs. Stack effects are written
`( inputs -- outputs )`, with the top of the stack on the right, so
`60 1 3 limb!` sets limb 1, muscle 3 to 60 %.

## The model in one paragraph

Each muscle has an **activation** of 0-100 %. The scheduler turns activations
into non-overlapping **on-windows** inside a repeating 20 ms frame, one wire on
at a time, and the windows are latched out through the 74HC595 chain to the
MOSFET gates. The window edges are timed by hardware timer GPT7 (3 MHz, edges
rounded to 24 us) and latched from its interrupts, not by the scheduler, so a
busy or stalled scheduler does not move them. A change reaches the wires at the
next frame start (up to 20 ms). `status` shows `frame timer: ok`; if the timer's
start-up self-test or its edge supervisor ever trips, it shows the fault code
and no pattern is latched. A muscle at 100 % gets `frame / (2 x fitted limbs)` of the frame
(12.5 % in the 4-limb build). Activating one side of an antagonist pair
releases the other.

Numbering: a muscle is 1-24, or limb 1-6 with muscle 1-4 within the limb,
`muscle = (limb - 1) x 4 + m`. Muscles within a limb:

| m | name | pair |
|---|---|---|
| 1 | shoulder forward | 1 <-> 2 |
| 2 | shoulder back | 1 <-> 2 |
| 3 | knee lift | 3 <-> 4 |
| 4 | knee bend | 3 <-> 4 |

The 4-limb build fits limbs 1, 2, 5, 6. Gait legs: LF = limb 1, RF = limb 2,
LR = limb 5, RR = limb 6. Shift register bit `b` of chip `c` is muscle
`c x 8 + b + 1`.

## Limb drive

| Word | Stack effect | What it does |
|---|---|---|
| `limb!` | `( pct limb m -- )` | Set limb 1-6, muscle 1-4 to `pct` 0-100. Releases the antagonist. |
| `muscle!` | `( pct muscle -- )` | Same, addressing the muscle 1-24 directly. |
| `relax` | `( -- )` | Set every muscle to 0 %. |
| `muscles` | `( -- )` | Show every fitted muscle's activation and its scheduled window in the frame. |
| `sr!` | `( value chip -- )` | Write a byte to shift register chip 0-2 and latch it. Bit 0 = QA ... bit 7 = QH. Bypasses the scheduler; the next window edge overwrites it. |
| `sr@` | `( chip -- value )` | The byte last written to that chip (the shadow, not a pin read). |

## Gaits

A gait runs continuously and sets the muscles itself. Change the gait, speed or
effort while it runs and it blends smoothly.

| Word | Stack effect | What it does |
|---|---|---|
| `crawl` | `( -- )` | Start or switch to crawl: one leg lifted at a time. |
| `trot` | `( -- )` | Trot: diagonal pairs. |
| `pace` | `( -- )` | Pace: same-side pairs. |
| `bound` | `( -- )` | Bound: front pair, then rear pair. |
| `halt` | `( -- )` | Stop the gait and relax every muscle. |
| `speed` | `( pct -- )` | Walking desire: 0 stands still, 100 is the fastest cycle. |
| `effort` | `( pct -- )` | Activation given to support and swing muscles (default 70). |
| `gait` | `( -- )` | Show gait state, each leg's phase and the gait tables. |
| `gait-trace` | `( -- )` | Toggle printing of each lift-off, touchdown, wait and reach. |
| `level` | `( -- )` | Toggle IMU leveling of the stance legs. |
| `posture` | `( -- )` | Show the IMU roll/pitch estimate and tip-over state. |

## Wire calibration

Finds where a wire's resistance changes as it heats, so activation can later be
mapped onto the wire's real working range. Results are duty in **tenths of a
percent** of the frame, saved in the `calib` file and restored at boot. Run
`halt` first: calibration fits only the wire's own limb while it runs.

| Word | Stack effect | What it does |
|---|---|---|
| `wirecal` | `( muscle -- )` | Calibrate one wire: rest, ramp its drive, measure R at each step, find onset, mid and full. Saves the result. |
| `wirecal-stop` | `( -- )` | Abort a running calibration and relax. |
| `wires` | `( -- )` | Show each calibrated wire: cold resistance (mohm) and onset, mid, full duty. |
| `wire!` | `( muscle rcold onset mid full -- )` | Set a wire's record directly. `rcold` in mohm, the rest in tenths of a percent duty. The calib file runs this at boot. |
| `calib-save` | `( -- )` | Write every registered calibration value to the calib file now. |

Example: `halt`, then `2 wirecal`, then `wires`.

## Sensing (supports the limb words)

The A3 divider (100K over `rbot!`) must keep the tap under about 4.5 V: at a 21 V
supply use about 18K, not the 47K used at 10 V. Set it with `rbot!` (ohms), or
give `vin-cal` the metered supply.

The sense taps and supply are what the wire calibration measures with.

| Word | Stack effect | What it does |
|---|---|---|
| `status` | `( -- )` | Read every connection: 595 control pins and outputs, the A0-A3 analog taps, the ADC reference, and the muscle windows. |
| `scope` | `( muscle -- )` | After that wire's next window opens, read the ADC back to back for up to 1.2 ms (at most three quarters of the window) and print time, current, supply and resistance for each sample: the waveform the ADC sees, to compare with a scope on the sense resistor. |
| `isns` | `( -- )` | Sample each active wire during its own on-window for 1 s; print sample count, peak current while on (min/avg/max mA), the wire resistance that implies, and the supply (min/avg/max mV). Use this to see pulse current: `status` reads the ADC at an arbitrary moment, mostly between pulses. |
| `adc@` | `( -- mv )` | Millivolts on AN000 (A1). |
| `vin@` | `( -- mv )` | Supply voltage in mV through the calibrated A3 divider. |
| `vin-cal` | `( mv -- )` | Give the metered supply in mV; solves the divider's bottom resistor and saves it. |
| `rbot!` | `( ohms -- )` | Set the divider's bottom resistor directly. The calib file runs this at boot. |
| `vref!` | `( mv -- )` | Fix the ADC reference (AVCC0) by hand and stop tracking. |
| `vref-auto` | `( -- )` | Toggle tracking AVCC0 from the internal reference (on by default). |
| `vref-cal` | `( mv -- )` | Give the metered AVCC0 in mV; calibrates the internal reference and saves it. |
| `vint!` | `( mv -- )` | Set the internal-reference calibration directly. The calib file runs this at boot. |
| `adcexp` | `( -- )` | Diagnostic: rerun the internal-reference reads under several scan settings. |

## Removed on the `pwm` branch

The analog driver is gone: the DAC0 words (`dac!`, `up`, `down`, `dactest`,
`dac-off`), the I2C limb array (`limbio`, `limbio!`, `limbio-assign`,
`limbio-assign1`, `limbio-reset1`, `limbon`, `limboff`, `i2ctest`, `i2cregs`,
`i2cseq`, `i2cverify`, `i2cpattern`, `i2charness`, `i2cscan`) and the
single-wire GPT PWM controller. They are still on `main`.
