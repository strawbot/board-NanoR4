# Nano R4 / RASC bring-up notes

Hard-won knowledge from porting TimbreOS to the Arduino Nano R4 (Renesas
RA4M1, FSP 6.2.0, RASC generator). Read this before re-running RASC or
porting to another RA-family board.

## Layout

| Item         | Location                   | Detail                                    |
|--------------|----------------------------|-------------------------------------------|
| User code    | `0x4000` onward            | Arduino DFU bootloader owns `0x0–0x3FFF`. |
| Vector table | `0x4000`                   | Placed in the 1 KB `FLASH_GAP` region.    |
| Code/rodata  | `0x4400` onward (239 KB)   | `FLASH` region.                           |
| RAM          | `0x20000000` – `0x20008000`| 32 KB.                                    |
| Data flash   | `0x40100000` – `0x40102000`| 8 KB.                                     |

`SCB->VTOR` is set to `&__VECTOR_TABLE` by FSP's `SystemInit()`. The linker
places `__VECTOR_TABLE` (16 system vectors) followed by `g_vector_table`
(NVIC IRQ vectors from `ra_gen/vector_data.c`) in `.fixed_vectors` +
`.application_vectors` inside `FLASH_GAP`. IRQ N is at
`VTOR + 0x40 + N*4`.

## Files RASC regenerates (and must be fixed up)

### `Nano/memory_regions.ld`

RASC regenerates this for a raw chip at `0x0` with 256 KB flash. For the
Nano R4 bootloader you must set:

```ld
RAM_START        = 0x20000000;
RAM_LENGTH       = 0x00008000;

FLASH_GAP_START  = 0x00004000;
FLASH_GAP_LENGTH = 0x00000400;
FLASH_START      = 0x00004400;
FLASH_LENGTH     = 0x0003BC00;

DATA_FLASH_START  = 0x40100000;
DATA_FLASH_LENGTH = 0x00002000;

/* Option settings live inside the read-only bootloader region. */
OPTION_SETTING_OFS0_LENGTH   = 0x00000000;
OPTION_SETTING_OFS1_LENGTH   = 0x00000000;
OPTION_SETTING_SECMPU_LENGTH = 0x00000000;
OPTION_SETTING_OSIS_LENGTH   = 0x00000000;
```

See the live file for the full comment block.

### `Nano/ra_gen/hal_data.c`

No hand-edits required. Everything that used to be a post-regen patch now
lives in one of three places:

1. **RASC configurator** — settings that RASC can express directly. Keep
   these set in the GUI (or equivalently in `configuration.xml`) so every
   regeneration comes out right:

| Module       | Field              | Required value                  | Why                                                                                                   |
|--------------|--------------------|---------------------------------|-------------------------------------------------------------------------------------------------------|
| `delta_timer`| `mode`             | `TIMER_MODE_ONE_SHOT`           | TEA scheduler arms the next alarm each time via `set_delta_alarm`.                                    |
| `delta_timer`| `cycle_end_ipl`    | `12` (or any 1..14, not 0)      | **IPL 0 is treated as "disabled" by the NVIC/BSP** — the ISR will never dispatch if left at 0.         |
| `delta_timer`| `count_up_source`  | `GPT_SOURCE_NONE`               | The `GPT_SOURCE_GPT_A/B/...` enums are *fixed hardware ELC slots*, NOT "pick channel A/B/...".         |
| `delta_timer`| `clear_source`     | `GPT_SOURCE_NONE`               | Same.                                                                                                  |
| `tick_timer` | `count_up_source`  | `GPT_SOURCE_NONE`               | See above.                                                                                             |
| `tick_timer` | `clear_source`     | `GPT_SOURCE_NONE`               | See above.                                                                                             |
| `g_rtc0`     | `clock_source`     | `RTC_CLOCK_SOURCE_LOCO`         | Nano R4 has NO 32.768 kHz sub-clock crystal (schematic only populates the 16 MHz main XO). LOCO is the only viable count source; drifts ±15% so wall time needs external correction. |
| `g_rtc0`     | `carry_ipl`        | `12` (or any 1..14, not 0)      | **IPL 0 is treated as "disabled"**; matches the GPT gotcha.                                            |
| bsp_cfg.h    | `BSP_CFG_RTC_USED` | `1`                             | Keep at 1 so FSP's RTC driver links; LOCO works without a sub-clock.                                   |
| bsp_cfg.h    | `BSP_CLOCK_CFG_SUBCLOCK_POPULATED` | `0`             | No 32.768 kHz XTAL on board. Leaving this at 1 makes BSP try to start SOSC (silently fails) and burns a pointless 1 s wait.                       |
| bsp_cfg.h    | `BSP_CLOCK_CFG_SUBCLOCK_STABILIZATION_MS` | `0`      | Irrelevant once POPULATED=0; zeroed for clarity.                                                       |

2. **`Board/clocks.c` → `init_clocks()`** — settings RASC can't pin down
   reliably. Currently this is the GPT prescaler (`source_div`) for both
   `tick_timer` and `delta_timer`: RASC picks the smallest divider that
   fits Period × Unit into the counter, which means changing the Period
   can silently shift the tick rate. `init_clocks()` takes a RAM-resident
   copy of each `timer_cfg_t` and overrides `source_div = 10` (/1024) plus
   `period_counts` / `duty_cycle_counts`. Nothing to restore after regen.

3. **`Board/init_clocks()` again — MSTP un-stop for the RTC.** Gotcha #7
   below: FSP's `R_RTC_Open()` does not call `R_BSP_MODULE_START`, so we
   do it ourselves right before `R_RTC_Open()`.

### `Nano/Board/project_defs.h`

`ONE_SECOND` must match the actual timer tick rate:

```c
#define ONE_SECOND (31250)   /* PCLKD/1024 @ 32 MHz = 31.25 kHz → 32 µs/tick */
```

## Recovery procedure after `RASC → Generate Project Content`

With the current setup this is almost a no-op — regeneration no longer
clobbers anything we rely on.

1. `git diff` to sanity-check that only RASC-generated files changed
   (`ra_gen/*`, `ra_cfg/*`, `memory_regions.ld`, `fsp_gen.ld`).
2. The linker reads `memory_regions_custom.ld` (ours, in git) instead of the
   regenerated `memory_regions.ld` (which is gitignored). Nothing to restore.
3. `pio run -t clean && pio run && pio run -t upload`.
4. Confirm the yellow LED (P204) heartbeat (two quick flashes + long pause)
   within ~1 s of reset and verify the CLI prompt over SCI2 (P301/P302).

If you add a new peripheral (ADC, DAC, more GPTs, AGT, comparators, …) in
RASC, remember gotcha #2: every newly-enabled interrupt defaults to IPL 0 in
the configurator, which is treated as "disabled". Bump it to a non-zero
priority before regenerating.

## Cortex-M4 / RA4M1 gotchas

1. **Arduino DFU bootloader hands off with `PRIMASK = 1`.** FSP's
   `SystemInit()` does not clear PRIMASK. Without an explicit
   `__enable_irq()` early in `hal_entry()`, IRQs stay globally masked and
   the scheduler freezes after the first dispatched action. The fix lives
   at the top of `Nano/src/hal_entry.c`.

2. **IPL 0 ≠ highest priority in FSP's BSP IRQ plumbing — it's treated as
   disabled.** Always use a non-zero `*_ipl` when you want the ISR to
   actually fire. `BSP_IRQ_DISABLED` is `0xFF`, but 0 also rejects the
   enable in practice (empirically verified: priority 0 + enabled bit in
   NVIC ISER still didn't dispatch the ISR on RA4M1 / FSP 6.2.0).

3. **`GPT_SOURCE_GPT_A … GPT_SOURCE_GPT_H` are fixed ELC slots, not a
   "route any channel" selector.** On RA4M1, cascading one GPT's overflow
   into another GPT via these enums does not work the way the name
   suggests. Use `source_div` (PCLKD prescaler) to get the tick rate you
   want, or route via a real ELC event.

4. **GPT0 is 32-bit; GPT1–GPT7 are 16-bit.** The 16-bit timers can only
   carry `0xFFFF` ticks (~2.097 s at PCLKD/1024); TEA's scheduler clamps
   and re-arms.

5. **`BSP_CORTEX_VECTOR_TABLE_ENTRIES == 16`** (the standard Cortex-M
   system vectors: SP, Reset, NMI, HardFault, MemManage, BusFault,
   UsageFault, 4×reserved, SVCall, DebugMonitor, reserved, PendSV,
   SysTick). The application vector table (`g_vector_table`) is placed
   immediately after, so IRQ 0 is at VTOR+0x40, IRQ N at VTOR+0x40+4N.

6. **Don't call newlib `mktime()`, `tzset()`, or `sscanf()` on bare-metal.**
   Observed symptom: `init_clocks()` never returns, firmware hangs silently
   between `boot_alive_blink()` and `usart_transport_init()` — no output,
   no heartbeat, no hint that the hang is inside the time helper.
   Root causes:
   - `mktime()` pulls in `tzset()` (which may read a `TZ` env var),
     retargetable locks, and in some link configurations `_sbrk` /
     `_gettimeofday`.
   - `sscanf()` drags in the full stdio FILE machinery, locale init, and
     in some builds `_malloc_r` / `_sbrk_r`.
   If any of those dependencies aren't fully satisfied at link time, the
   call spins forever. Use hand-rolled arithmetic instead: a fixed-offset
   positional parser for `__TIMESTAMP__` and Howard Hinnant's closed-form
   `days_from_civil` for the civil-to-epoch conversion. See
   `timestamp_to_utc()` in `Board/clocks.c`.

7. **`R_RTC_Open()` does NOT start the RTC module (MSTP) — unlike every
   other FSP driver.** Verified against FSP 6.2.0 `ra/fsp/src/r_rtc/r_rtc.c`:
   zero `R_BSP_MODULE_START` references. Meanwhile `r_gpt.c`, `r_sci_uart.c`,
   `r_elc.c`, `r_dtc.c`, `r_dmac.c` all call `R_BSP_MODULE_START(FSP_IP_xxx,
   channel)` at the top of their `_Open()`.
   On RA4M1, `MSTPCRD[23]` (RTC) defaults to 1 (module stopped) after reset.
   While stopped, writes to `R_RTC->RCRn` are silently dropped and reads
   return 0, which makes every `FSP_HARDWARE_REGISTER_WAIT` inside
   `r_rtc_set_clock_source()` → `r_rtc_start_bit_update()` spin forever.
   Symptom: `init_clocks()` never returns, no serial, no heartbeat.
   Fix: call `R_BSP_MODULE_START(FSP_IP_RTC, 0);` yourself before
   `R_RTC_Open(&g_rtc0_ctrl, &g_rtc0_cfg)`. See `init_clocks()` in
   `Board/clocks.c`.

8. **Nano R4 has NO 32.768 kHz sub-clock crystal.** The Arduino schematic
   only populates the 16 MHz main XO. Do not trust RASC's default
   `RTC_CLOCK_SOURCE_SUBCLK` or `BSP_CLOCK_CFG_SUBCLOCK_POPULATED=1`.
   Selecting SUBCLK makes `R_RTC_Open()` hang forever in FSP's
   `r_rtc_set_clock_source()` — specifically at
   `FSP_HARDWARE_REGISTER_WAIT(R_RTC->RCR2_b.START, 0)`, which synchronizes
   to a tick of the selected count source. With no crystal, SOSC never
   ticks, the wait is infinite, and the firmware hangs silently after the
   MSTP un-stop (gotcha #7) succeeds.
   Fix: use LOCO as the RTC count source instead (`.clock_source =
   RTC_CLOCK_SOURCE_LOCO` in `g_rtc0_cfg`) and set
   `BSP_CLOCK_CFG_SUBCLOCK_POPULATED = 0` so BSP doesn't waste a second
   trying to start a phantom oscillator. LOCO drifts ±15% per datasheet,
   so wall-clock UTC needs periodic correction from an external source
   (NTP, GPS, CLI `set-utc`).
   Bisection pattern that caught this: `RTC_BRINGUP_STAGE` in
   `Board/clocks.c` (stages 0–4 add MSTP, Open, CalendarTimeGet,
   CalendarTimeSet incrementally). Stage 1 passed, stage 2 hung → the bad
   call was `R_RTC_Open()` itself, which pointed to the count source.

## Build / flash

```sh
cd Nano
pio run -t clean
pio run
pio run -t upload          # DFU over USB; board must be in bootloader mode
```

To put the Nano R4 into DFU bootloader mode: double-tap the reset button
within ~0.5 s. The bootloader advertises VID 0x2341 / PID 0x0374 and
exposes `8*2Ka` (16 KB) of the flash as read-only.

## Diagnostics that worked during bring-up

For future debugging sessions, these bisection techniques proved
invaluable and are worth re-introducing temporarily:

- **`diag_pulse(N)`**: N short LED flashes after a long pause — easy to
  visually count "how many init steps completed before we hung".
- **`init_clocks_diag_pulse2()` pattern**: two quick pulses (happy) vs.
  one long pulse (sad) for yes/no checks.
- **Manual NVIC pend test**: call `NVIC_SetPendingIRQ(N)` and watch for
  the ISR. Separates NVIC/vector-table issues from ICU IELSR routing
  issues.
- **Read-back diagnostics**: read `SCB->VTOR`, `NVIC->ISER[0]`,
  `__get_PRIMASK()`, `__get_IPSR()`, and the vector content at
  `VTOR + 0x40 + N*4` — each pulsed out via the LED as pass/fail.
  This is what caught the Arduino PRIMASK quirk.

These were all removed for the shipping build but are preserved in git
history if you need them again.
