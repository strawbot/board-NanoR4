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
#include "calib.h"

// AVCC0 is the ADC reference. On this board it comes from USB 5 V through
// the Nano's supply path, so it sits below 5 V and moves with the USB
// supply. 4696 mV = the Nano's 5 V pin measured with a meter; set it at run
// time with `vref!` when it drifts.
#define ADC_VREF_DEFAULT_MV 4696UL
static uint32_t adc_vref_mv = ADC_VREF_DEFAULT_MV;
#define ADC_VREF_MV    adc_vref_mv

// Automatic tracking: the internal reference (about 1.43 V, fixed) is scanned
// with the pins, so raw_ref = Vint/AVCC0 * 16383 and AVCC0 = Vint*16383/raw_ref.
// ADC_VINT_DEFAULT_MV is this unit's reference calibrated once against a meter
// (raw 5004 at a metered AVCC0 of 4696 mV); the datasheet only promises
// 1360-1500 mV, so recalibrate with `vref-cal` on another board. The runtime
// value is not persisted.
#define ADC_VINT_DEFAULT_MV 1434UL
static uint32_t adc_vint_mv = ADC_VINT_DEFAULT_MV;
static bool     vref_auto;
static uint32_t ref_raw_f;      // low-pass filtered reference raw, x16; 0 = none yet
#define ADC_FULL_SCALE 16383UL   // 2^14 - 1

// ── ADC instance (not generated; defined locally) ────────────────────────────

static adc_instance_ctrl_t g_board_adc_ctrl;   // DO NOT pre-initialize (FSP requirement)

static const adc_extended_cfg_t g_board_adc_cfg_extend = {
    .add_average_count   = ADC_ADD_OFF,
    .clearing            = ADC_CLEAR_AFTER_READ_ON,
    // Software start needs ADC_START_SOURCE_DISABLED. Left unset it is 0 =
    // ADC_START_SOURCE_ASYNC_EXTERNAL: the ADC then waits on the ADTRG pin
    // and ADST is never set, so every channel reads 0.
    .trigger             = ADC_START_SOURCE_DISABLED,
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

// Scan AN000 (P000), AN001 (P001), AN002 (P002), AN009 (P014) and the
// internal reference together.
// Hex-board plan: A0=AN009 ISNS_A, A1=AN000 ISNS_B, A2=AN001 ISNS_C,
// A3=AN002 VIN_S.
// The internal reference is scanned with the pins; the temperature sensor is
// deliberately NOT: with both sensors in one scan the reference register
// reads the temperature value (bench-verified with `adcexp`, with and without
// pin channels), while reference + pins alone reads correctly.
#define ADC_PIN_MASK    ((1U << 0) | (1U << 1) | (1U << 2) | (1U << 9))
#define ADC_NORMAL_MASK (ADC_PIN_MASK | ADC_MASK_VOLT)
#define ADC_ALL_MASK    (ADC_NORMAL_MASK | ADC_MASK_TEMPERATURE)   // known-bad, for adcexp

static const adc_channel_cfg_t g_board_adc_channel_cfg = {
    .scan_mask          = ADC_NORMAL_MASK,
    .scan_mask_group_b  = 0U,
    .add_mask           = 0U,
    .p_window_cfg       = NULL,
    .priority_group_a   = ADC_GROUP_A_PRIORITY_OFF,
    .sample_hold_mask   = 0U,   // 0 = no channel uses sample_hold_states below;
    .sample_hold_states = 24U,  // dead field as configured — reviewed for the 48MHz clock bump, nothing to change since it isn't applied to any channel.
};

// ── Init / read API ──────────────────────────────────────────────────────────

// First non-success FSP code seen during adc_init (0 = all good); shown by
// the `status` word.
static fsp_err_t adc_init_err;

int adc_init_status(void) { return (int)adc_init_err; }

void adc_init(void) {
    // P000/P001 are analog from ra_gen/pin_data.c; P002 and P014 are not.
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_02, (uint32_t)IOPORT_CFG_ANALOG_ENABLE);
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_14, (uint32_t)IOPORT_CFG_ANALOG_ENABLE);

    adc_init_err = R_ADC_Open(&g_board_adc_ctrl, &g_board_adc_cfg);
    fsp_err_t e = R_ADC_ScanCfg(&g_board_adc_ctrl, &g_board_adc_channel_cfg);
    if (!adc_init_err) adc_init_err = e;

    // Longest sample time (255 states) on every scanned channel. The taps
    // are high impedance (100K/47K divider ~32k, sense taps behind a 1k
    // filter); the default sample time is meant for a few kOhm and reads
    // such a source low.
    static const uint8_t scanned[] = { 0, 1, 2, 9 };
    for (unsigned i = 0; i < sizeof scanned; i++) {
        adc_sample_state_t ss = { (adc_sample_state_reg_t)scanned[i], 255U };
        e = R_ADC_SampleStateCountSet(&g_board_adc_ctrl, &ss);
        if (!adc_init_err) adc_init_err = e;
    }

    e = R_ADC_ScanStart(&g_board_adc_ctrl);
    if (!adc_init_err) adc_init_err = e;
}

// Any scanned channel's raw 14-bit code.
uint16_t adc_read_an_raw(uint8_t an) {
    uint16_t raw = 0;
    R_ADC_Read(&g_board_adc_ctrl, (adc_channel_t)an, &raw);
    return raw;
}

// Any scanned channel (an = ANxxx number) in millivolts (0–5000).
uint32_t adc_read_an_mv(uint8_t an) {
    uint16_t raw = 0;
    R_ADC_Read(&g_board_adc_ctrl, (adc_channel_t)an, &raw);
    return (uint32_t)raw * ADC_VREF_MV / ADC_FULL_SCALE;
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

// Sanity readout that needs no external wiring: the ADC control registers and
// the internal reference (datasheet Table 2.50: 1.36 - 1.50 V, typ 1.43 V).
// The reference reads Vint/AVCC0*16383 counts (~5000 at 4.7 V); its implied
// voltage below should land inside that range when AVCC0 (`vref!`) is right.
// The temperature sensor is not in the scan (see ADC_NORMAL_MASK).
void adc_print_diag(void) {
    R_ADC0_Type *r = g_board_adc_ctrl.p_reg;
    print("  ADCSR=");   printHex4(r->ADCSR);
    print(" ADCER=");    printHex4(r->ADCER);
    print(" ADANSA0=");  printHex4(r->ADANSA[0]);
    print(" ADEXICR=");  printHex4(r->ADEXICR);
    printCr();
    uint32_t ref    = r->ADOCDR;
    uint32_t ref_mv = ref * ADC_VREF_MV / ADC_FULL_SCALE;
    print("  internal ref raw "); printDec0(ref);
    print(" = ");                printDec0(ref_mv); print(" mV");
    if (vref_auto) print("  (auto: the reference defines AVCC0)");
    else print(ref_mv >= 1360U && ref_mv <= 1500U ? "  (in datasheet range 1360-1500)"
                                                  : "  (OUTSIDE datasheet range 1360-1500)");
    if (ref) { print("  implied AVCC0 @1.43V "); printDec0(1430U * ADC_FULL_SCALE / ref); print(" mV"); }
    printCr();
}

uint32_t adc_vref_get(void) { return adc_vref_mv; }

#define VREF_TRACK_MS 100

static void vref_track(void) {
    // ADOCDR clears on read, so a 0 just means no fresh conversion: skip it.
    uint32_t raw = g_board_adc_ctrl.p_reg->ADOCDR;
    if (raw > 1000U) {
        int32_t f = (int32_t)ref_raw_f;
        f = f ? f + ((int32_t)(raw * 16U) - f) / 8 : (int32_t)(raw * 16U);
        ref_raw_f = (uint32_t)f;
        if (vref_auto) {
            uint32_t v = adc_vint_mv * ADC_FULL_SCALE * 16U / ref_raw_f;
            adc_vref_mv = v < 3000U ? 3000U : v > 5500U ? 5500U : v;
        }
    }
    if (vref_auto) in(msec(VREF_TRACK_MS), vref_track);
}

static int32_t vint_get(void) { return (int32_t)adc_vint_mv; }

// CLI word: ( mv -- ) set the internal reference calibration directly. This is
// the word the calibration file runs at boot (see Robot/storage/calib).
void adc_cli_vint_set(void) {
    Cell v = ret();
    if (v >= 1000 && v <= 2000) adc_vint_mv = (uint32_t)v;
}

// Start tracking AVCC0 from the internal reference. Call after init_tea().
void adc_vref_start(void) {
    static bool named;
    if (!named) {
        named = true;
        namedAction(vref_track);
        calib_register("vint!", vint_get);   // saved by vref-cal, restored by calib_load
    }
    if (vref_auto) return;
    vref_auto = true;
    vref_track();
}

// CLI word: ( mv -- ) fix the ADC reference by hand; turns tracking off.
void adc_cli_vref_set(void) {
    Cell v = ret();
    if (v < 3000) v = 3000;
    if (v > 5500) v = 5500;
    vref_auto = false;
    stop(vref_track);
    adc_vref_mv = (uint32_t)v;
}

// CLI word: toggle automatic AVCC0 tracking from the internal reference.
void adc_cli_vref_auto(void) {
    if (vref_auto) { vref_auto = false; stop(vref_track); }
    else adc_vref_start();
}

// CLI word: ( mv -- ) calibrate the internal reference against a metered AVCC0.
void adc_cli_vref_cal(void) {
    Cell v = ret();
    if (v < 3000 || v > 5500) { print("vref-cal: give AVCC0 in mV, 3000-5500"); printCr(); return; }
    if (!ref_raw_f)            { print("vref-cal: no reference reading yet"); printCr(); return; }
    adc_vint_mv = ref_raw_f * (uint32_t)v / (ADC_FULL_SCALE * 16U);
    print("vref-cal: internal reference = "); printDec0(adc_vint_mv); print(" mV"); printCr();
    calib_save();
}

// One-line reference status for `status`.
void adc_print_vref(void) {
    print("ADC reference "); printDec0(adc_vref_mv); print(" mV  ");
    if (vref_auto) { print("(auto, internal ref "); printDec0(adc_vint_mv);
                     print(" mV; vref-auto to stop, vref-cal to recalibrate)"); }
    else           { print("(manual; vref-auto to track the internal reference)"); }
    printCr();
}

// ── `adcexp`: why does the internal reference read low? ─────────────────────
//
// Reruns the internal-reference / temperature-sensor reads under several
// configurations (scanned alone or with every channel, FSP's sample time or
// forced ones) and prints each. If the reference reading moves with sample
// time, or with what else is scanned, the low value is a settling / channel-
// interaction artefact; if it never moves, the reading is something else.
// Runs cooperatively; restores the normal scan config at the end.

typedef struct { uint32_t mask; uint8_t states; const char *name; } exp_trial_t;

static const exp_trial_t exp_trials[] = {
    { ADC_ALL_MASK,            0,   "pins + both sensors (known-bad), FSP sample time" },
    { ADC_MASK_VOLT,           0,   "reference only, FSP sample time" },
    { ADC_MASK_VOLT,           255, "reference only, 255 states" },
    { ADC_MASK_VOLT,           30,  "reference only, 30 states" },
    { ADC_MASK_TEMPERATURE,    0,   "temperature only, FSP sample time" },
    { ADC_ALL_MASK,            255, "pins + both sensors (known-bad), 255 states" },
    { ADC_NORMAL_MASK,         0,   "pins + reference (the normal scan)" },
    { ADC_PIN_MASK | ADC_MASK_TEMPERATURE,     0, "pins + temperature (no reference)" },
    { ADC_MASK_VOLT | ADC_MASK_TEMPERATURE,    0, "reference + temperature, no channels" },
};
#define EXP_TRIALS (sizeof exp_trials / sizeof exp_trials[0])
#define EXP_SAMPLES 8U

static uint8_t  exp_trial, exp_n;
static bool     exp_running, exp_named;
static uint32_t ref_sum, ts_sum, ref_min, ref_max;

static void exp_apply(uint32_t mask, uint8_t states) {
    R_ADC0_Type *r = g_board_adc_ctrl.p_reg;
    adc_channel_cfg_t c = g_board_adc_channel_cfg;
    c.scan_mask = mask;
    R_ADC_ScanStop(&g_board_adc_ctrl);
    R_ADC_ScanCfg(&g_board_adc_ctrl, &c);          // FSP sets sensor sample states
    if (states) { r->ADSSTRT = states; r->ADSSTRO = states; }
    R_ADC_ScanStart(&g_board_adc_ctrl);
}

static void exp_sample(void);

static void exp_begin(void) {
    exp_apply(exp_trials[exp_trial].mask, exp_trials[exp_trial].states);
    exp_n = 0; ref_sum = ts_sum = 0; ref_min = 0xFFFF; ref_max = 0;
    in(msec(5), exp_sample);                       // let the first scan finish
}

static void exp_sample(void) {
    R_ADC0_Type *r = g_board_adc_ctrl.p_reg;
    uint32_t ref = r->ADOCDR, ts = r->ADTSDR;
    ref_sum += ref; ts_sum += ts;
    if (ref < ref_min) ref_min = ref;
    if (ref > ref_max) ref_max = ref;
    if (++exp_n < EXP_SAMPLES) { in(msec(2), exp_sample); return; }

    uint32_t ref_avg = ref_sum / EXP_SAMPLES, ts_avg = ts_sum / EXP_SAMPLES;
    print("  "); print(exp_trials[exp_trial].name); printCr();
    print("      ADSSTRO/T "); printDec0(r->ADSSTRO); print("/"); printDec0(r->ADSSTRT);
    print("   ref raw ");   printDec0(ref_avg);
    print(" (");            printDec0(ref_min); print("-"); printDec0(ref_max); print(")");
    if (ref_avg) { print("  implied AVCC0 @1.43V ");
                   printDec0(1430U * ADC_FULL_SCALE / ref_avg); print(" mV"); }
    print("   temp raw ");  printDec0(ts_avg);
    printCr();

    if (++exp_trial < EXP_TRIALS) { exp_begin(); return; }
    exp_apply(ADC_NORMAL_MASK, 0);                 // back to the normal scan
    exp_running = false;
    print("adcexp: done, normal scan restored"); printCr();
}

void adc_cli_experiment(void) {
    if (exp_running) { print("adcexp: already running"); printCr(); return; }
    if (!exp_named) { exp_named = true; namedAction(exp_sample); }
    R_ADC0_Type *r = g_board_adc_ctrl.p_reg;
    print("adcexp: ADC clock "); printDec0(R_FSP_SystemClockHzGet(BSP_FEATURE_ADC_CLOCK_SOURCE));
    print(" Hz, AVCC0 "); printDec0(adc_vref_mv);
    print(" mV -> a 1.43 V reference should read raw ");
    printDec0(1430U * ADC_FULL_SCALE / adc_vref_mv); printCr();
    print("  ADCER="); printHex4(r->ADCER); print(" ADCSR="); printHex4(r->ADCSR); printCr();
    exp_trial = 0; exp_running = true;
    exp_begin();
}
