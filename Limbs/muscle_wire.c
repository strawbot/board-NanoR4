// Limbs/muscle_wire.c — digital PWM muscle-wire driver
//
// Replaces the original DAC/op-amp version with a PWM+PI current controller.
// All timing is driven by the TEA scheduler; there are no blocking delays.
//
// Hardware
// ────────
//   P112  GTIOC3B  →  PWM output to MOSFET gate (via 100 Ω)
//   P001  AN001    →  1 Ω sense resistor, read via ADC continuous scan
//
// FSP configuration required (RASC / e2 studio)
// ──────────────────────────────────────────────
//   • Add a GPT3 instance in PWM mode with GTIOCB output on P112.
//     This file defines the instance manually (g_mw_pwm_*) so you do
//     NOT need to regenerate hal_data — but you MUST configure the pin:
//       Ports → P112 → Mode = Peripheral output, PSEL = GTIOC3B
//   • AN001 (P001) is added to the ADC scan in Board/adc.c — no RASC
//     change needed for the ADC.
//
// TEA integration
// ───────────────
//   calibrate_muscle_wire()  — self-rescheduling state machine.
//                              Call once to (re-)run calibration.
//   set_muscle_wire(p)       — set activation 0–100 after calibration.
//   pi_update()              — 1 ms recurring action, started by calibration.
//   muscle_wire_init()       — open GPT, call from hal_entry before init_tea.

#include "tea.h"
#include "cli.h"
#include "printers.h"
#include "hal_data.h"
#include <stdint.h>

// ── ADC sense read (Board/adc.c) ─────────────────────────────
float adc_read_sense_ma(void);

// ── Configuration ─────────────────────────────────────────────
#define MW_PWM_PERIOD       400         // duty steps (= counter period)
#define MW_I_THRESHOLD      5.0f        // mA — onset of conduction
#define MW_I_MAX            550.0f      // mA — hard ceiling / plateau trigger
#define MW_PLATEAU_STEPS    5           // consecutive low-slope steps → plateau
#define MW_TAU_S            0.010f      // RC filter τ (1 kΩ × 10 µF)
#define MW_LAMBDA_S         0.010f      // initial closed-loop τ
#define MW_OVERSHOOT_LIMIT  10.0f       // % overshoot before re-tune
#define MW_MAX_RETRY        3

// ── GPT3 PWM instance for P112 (GTIOC3B) ─────────────────────
// Period: 400 counts at PCLKD/8 (32 MHz/8 = 4 MHz) → 10 kHz PWM.
// source_div = 3 means divide-by-2^3 = /8 (raw exponent, not FSP enum).
// P112 configured as GTIOC3B in RASC / pin_data.c.

static gpt_instance_ctrl_t g_mw_pwm_ctrl;

static const gpt_extended_cfg_t g_mw_pwm_extend = {
    .gtioca = { .output_enabled = false,
                .stop_level     = GPT_PIN_LEVEL_LOW },
    .gtiocb = { .output_enabled = true,
                .stop_level     = GPT_PIN_LEVEL_LOW },
    .start_source        = (gpt_source_t)(GPT_SOURCE_NONE),
    .stop_source         = (gpt_source_t)(GPT_SOURCE_NONE),
    .clear_source        = (gpt_source_t)(GPT_SOURCE_NONE),
    .count_up_source     = (gpt_source_t)(GPT_SOURCE_NONE),
    .count_down_source   = (gpt_source_t)(GPT_SOURCE_NONE),
    .capture_a_source    = (gpt_source_t)(GPT_SOURCE_NONE),
    .capture_b_source    = (gpt_source_t)(GPT_SOURCE_NONE),
    .capture_a_ipl       = BSP_IRQ_DISABLED,
    .capture_b_ipl       = BSP_IRQ_DISABLED,
    .capture_a_irq       = FSP_INVALID_VECTOR,
    .capture_b_irq       = FSP_INVALID_VECTOR,
    .compare_match_value = { (uint32_t)0x0, (uint32_t)0x0 },
    .compare_match_status = 0U,
    .capture_filter_gtioca = GPT_CAPTURE_FILTER_NONE,
    .capture_filter_gtiocb = GPT_CAPTURE_FILTER_NONE,
    .p_pwm_cfg           = NULL,
    .gtior_setting.gtior = 0U,
    .gtioca_polarity     = GPT_GTIOC_POLARITY_NORMAL,
    .gtiocb_polarity     = GPT_GTIOC_POLARITY_NORMAL,
};

static const timer_cfg_t g_mw_pwm_cfg = {
    .mode               = TIMER_MODE_PWM,
    .period_counts      = (uint32_t)MW_PWM_PERIOD,
    .duty_cycle_counts  = 0U,
    .source_div         = (timer_source_div_t)3,  // /8 → 4 MHz → 10 kHz
    .channel            = 3,                       // GPT3; GTIOC3A = P003
    .p_callback         = NULL,
    .p_context          = NULL,
    .p_extend           = &g_mw_pwm_extend,
    .cycle_end_ipl      = BSP_IRQ_DISABLED,
    .cycle_end_irq      = FSP_INVALID_VECTOR,
};

// ── PWM write helper ──────────────────────────────────────────
static void pwm_set(Short duty) {
    if (duty < 0)              duty = 0;
    if (duty > MW_PWM_PERIOD)  duty = MW_PWM_PERIOD;
    R_GPT_DutyCycleSet(&g_mw_pwm_ctrl, (uint32_t)duty, GPT_IO_PIN_GTIOCB);
}

// ── Calibration state and results ────────────────────────────
typedef enum { MW_IDLE, MW_RAMP, MW_K_SETTLE, MW_K_MEASURE,
               MW_VAL_SETTLE, MW_VAL_MEASURE } mw_cal_state_t;

static mw_cal_state_t cal_state = MW_IDLE;
static bool  calibrated  = false;

// Ramp-phase state
static Short cal_duty    = 0;
static Short plateau_n   = 0;
static float I_prev      = 0.0f;

// Calibration results
static Short dc_low      = 0;
static Short dc_high     = 0;
static float I_low_ma    = 0.0f;
static float I_high_ma   = 0.0f;

// K-measurement state
static float K_before    = 0.0f;   // I before step
static float K_peak      = 0.0f;   // peak I after step → I_final
static Short k_samples   = 0;

// Validation state
static Short val_samples = 0;
static float val_peak    = 0.0f;
static Short tune_retry  = 0;
static float lambda_s    = MW_LAMBDA_S;

// ── PI state ─────────────────────────────────────────────────
static float K           = 0.001f; // process gain [mA / duty step]
static float Kp          = 0.0f;
static float Ki          = 0.0f;
static float integral    = 0.0f;
static float I_target_ma = 0.0f;

// ── Gain formula (IMC) ────────────────────────────────────────
static void compute_gains(void) {
    Kp = MW_TAU_S / (K * lambda_s);
    Ki = Kp / MW_TAU_S;
}

// ── Step-test geometry helpers ────────────────────────────────
static Short k_step(void) {
    Short s = (dc_high - dc_low) / 10;
    return s < 2 ? 2 : s;
}
static Short k_base(void) {
    Short b = dc_low + (dc_high - dc_low) / 2 - k_step() / 2;
    return b < dc_low ? dc_low : b;
}

// ── Inline PI step (used during validation phases) ───────────
// Returns the new duty cycle applied.
static Short pi_step(float I) {
    float error   = I_target_ma - I;
    float max_int = (Ki > 1e-9f) ? ((float)MW_PWM_PERIOD / Ki) : (float)MW_PWM_PERIOD;
    integral += error * 0.001f;                         // dt = 1 ms
    if (integral >  max_int) integral =  max_int;
    if (integral < -max_int) integral = -max_int;
    float ff   = I_target_ma / K;                       // feedforward
    float duty = ff + Kp * error + Ki * integral;
    if (duty < 0.0f)              duty = 0.0f;
    if (duty > (float)MW_PWM_PERIOD) duty = (float)MW_PWM_PERIOD;
    Short d = (Short)duty;
    pwm_set(d);
    return d;
}

// ── pi_update — 1 ms recurring TEA action ────────────────────
// Started by calibration on completion; self-rescheduling.
void pi_update(void) {
    if (I_target_ma <= 0.0f) {
        pwm_set(0);
        integral = 0.0f;
    } else {
        pi_step(adc_read_sense_ma());
    }
    after(msec(1), pi_update);
}

// ── calibrate_muscle_wire — TEA state machine ─────────────────
// Call to start (or restart) calibration.
// Runs entirely via after(); never blocks.
void calibrate_muscle_wire(void) {
    float I = adc_read_sense_ma();

    switch (cal_state) {

    // ── Reset everything and begin ramp ──────────────────────
    case MW_IDLE:
        calibrated = false;
        stop(pi_update);
        dc_low = dc_high = 0;
        I_low_ma = I_high_ma = 0.0f;
        K = 0.001f;  Kp = 0.0f;  Ki = 0.0f;
        plateau_n = 0;  I_prev = 0.0f;  cal_duty = 0;
        lambda_s = MW_LAMBDA_S;  tune_retry = 0;
        pwm_set(0);
        print("\ncal: ramp");
        cal_state = MW_RAMP;
        after(msec(10), calibrate_muscle_wire);
        return;

    // ── Ramp: step duty every 10 ms, watch for onset + plateau ──
    case MW_RAMP:
        // Onset detection
        if (!dc_low && I >= MW_I_THRESHOLD) {
            dc_low   = cal_duty;
            I_low_ma = I;
        }
        if (dc_low) {
            if (I > I_high_ma) { I_high_ma = I;  dc_high = cal_duty; }
            float slope = I - I_prev;
            bool  flat  = (I >= MW_I_MAX) || (cal_duty > dc_low && slope < 0.5f);
            plateau_n   = flat ? plateau_n + 1 : 0;
        }
        I_prev = I;

        // End-of-ramp conditions
        if ((dc_low && plateau_n >= MW_PLATEAU_STEPS) || cal_duty >= MW_PWM_PERIOD) {
            if (!dc_low || dc_high <= dc_low) {
                pwm_set(0);
                print("\ncal: FAILED (no range)");
                cal_state = MW_IDLE;
                return;
            }
            // Settle at mid-range before step test
            pwm_set(k_base());
            cal_state = MW_K_SETTLE;
            after(msec(80), calibrate_muscle_wire);   // > 5τ
            return;
        }
        pwm_set(++cal_duty);
        after(msec(10), calibrate_muscle_wire);
        return;

    // ── K settle: record I_before, fire the step ─────────────
    case MW_K_SETTLE:
        K_before  = I;
        K_peak    = I;
        k_samples = 0;
        pwm_set(k_base() + k_step());
        cal_state = MW_K_MEASURE;
        after(msec(1), calibrate_muscle_wire);
        return;

    // ── K measure: sample 60 ms, find I_final, compute gains ─
    case MW_K_MEASURE:
        if (I > K_peak) K_peak = I;
        if (++k_samples < 60) {
            after(msec(1), calibrate_muscle_wire);
            return;
        }
        {
            float dI = K_peak - K_before;
            float dD = (float)k_step();
            K = (dI > 0.0f && dD > 0.0f) ? dI / dD : 0.001f;
            compute_gains();
        }
        // Begin validation: settle PI at 25 % target
        I_target_ma = I_low_ma + 0.25f * (I_high_ma - I_low_ma);
        integral    = 0.0f;
        val_samples = 0;
        cal_state   = MW_VAL_SETTLE;
        after(msec(1), calibrate_muscle_wire);
        return;

    // ── Val settle: run PI for 150 ms at 25 % ────────────────
    case MW_VAL_SETTLE:
        pi_step(I);
        if (++val_samples < 150) {
            after(msec(1), calibrate_muscle_wire);
            return;
        }
        // Step to 75 % and start measuring
        I_target_ma = I_low_ma + 0.75f * (I_high_ma - I_low_ma);
        val_peak    = 0.0f;
        val_samples = 0;
        cal_state   = MW_VAL_MEASURE;
        after(msec(1), calibrate_muscle_wire);
        return;

    // ── Val measure: run PI for 250 ms, track peak ───────────
    case MW_VAL_MEASURE:
        pi_step(I);
        if (I > val_peak) val_peak = I;
        if (++val_samples < 250) {
            after(msec(1), calibrate_muscle_wire);
            return;
        }
        // Check overshoot; retune lambda if needed
        {
            float I_75     = I_low_ma + 0.75f * (I_high_ma - I_low_ma);
            float span     = I_high_ma - I_low_ma;
            float overshoot = (span > 0.0f) ? 100.0f * (val_peak - I_75) / span : 0.0f;
            if (overshoot > MW_OVERSHOOT_LIMIT && tune_retry < MW_MAX_RETRY) {
                tune_retry++;
                lambda_s *= 1.5f;
                if (lambda_s > 4.0f * MW_TAU_S) lambda_s = 4.0f * MW_TAU_S;
                compute_gains();
                // Retry from settle phase
                I_target_ma = I_low_ma + 0.25f * (I_high_ma - I_low_ma);
                integral    = 0.0f;
                val_samples = 0;
                val_peak    = 0.0f;
                cal_state   = MW_VAL_SETTLE;
                after(msec(1), calibrate_muscle_wire);
                return;
            }
        }
        // ── Calibration complete ──────────────────────────────
        pwm_set(0);
        I_target_ma = 0.0f;
        integral    = 0.0f;
        calibrated  = true;
        cal_state   = MW_IDLE;     // next call restarts from scratch
        print("\ncal ok");
        print("  dc_low=");   printDec(dc_low);
        print("  dc_high=");  printDec(dc_high);
        print("  K=");        printFloat0(K,  3);
        print("  Kp=");       printFloat0(Kp, 4);
        print("  Ki=");       printFloat0(Ki, 2);
        print("  lam_ms=");   printDec((Short)(lambda_s * 1000.0f));
        printCr();
        after(msec(1), pi_update);   // hand off to runtime PI loop
        return;
    }
}

// ── Public API ────────────────────────────────────────────────

// Set activation level 0–100.  Ignored until calibration is complete.
// 0 = fully off; 100 = full activation (I_high_ma).
// Intermediate values interpolate linearly across the calibrated range.
void set_muscle_wire(Short p) {
    if (!calibrated) return;
    if (p <= 0) {
        I_target_ma = 0.0f;
    } else if (p >= 100) {
        I_target_ma = I_high_ma;
    } else {
        I_target_ma = I_low_ma + (float)p / 100.0f * (I_high_ma - I_low_ma);
    }
}

// Open and start the GPT PWM timer.  Call once from hal_entry(),
// before init_tea(), so that pwm_set(0) silences the output safely.
void muscle_wire_init(void) {
    R_GPT_Open(&g_mw_pwm_ctrl, &g_mw_pwm_cfg);
    R_GPT_Start(&g_mw_pwm_ctrl);
    pwm_set(0);
}

// ── CLI words ─────────────────────────────────────────────────
void cli_calibrate_muscle_wire(void) { calibrate_muscle_wire(); }
void cli_set_muscle_wire(void)        { set_muscle_wire((Short)ret()); }
