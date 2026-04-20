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

RASC regenerates this from the RASC configurator. Several values must be
hand-patched (or set correctly in RASC itself — which is the better long-term
fix). The critical settings:

| Module       | Field              | Required value                  | Why                                                                                                   |
|--------------|--------------------|---------------------------------|-------------------------------------------------------------------------------------------------------|
| `delta_timer`| `mode`             | `TIMER_MODE_ONE_SHOT`           | TEA scheduler arms the next alarm each time via `set_delta_alarm`.                                    |
| `delta_timer`| `source_div`       | `10` (i.e. /1024)               | PCLKD/1024 @ 32 MHz → 31.25 kHz tick, same unit as `tick_timer`.                                      |
| `delta_timer`| `cycle_end_ipl`    | `12` (or any 1..14, not 0)      | **IPL 0 is treated as "disabled" by the NVIC/BSP** — the ISR will never dispatch if left at 0.         |
| `delta_timer`| `count_up_source`  | `GPT_SOURCE_NONE`               | The `GPT_SOURCE_GPT_A/B/...` enums are *fixed hardware ELC slots*, NOT "pick channel A/B/...".         |
| `delta_timer`| `clear_source`     | `GPT_SOURCE_NONE`               | Same.                                                                                                  |
| `tick_timer` | `source_div`       | `10` (i.e. /1024)               | 32 µs/tick matches `ONE_SECOND = 31250` in `Board/project_defs.h`.                                     |
| `tick_timer` | `count_up_source`  | `GPT_SOURCE_NONE`               | See above.                                                                                             |
| `tick_timer` | `clear_source`     | `GPT_SOURCE_NONE`               | See above.                                                                                             |

After regenerating, `grep '/\* HAND-EDIT' ra_gen/hal_data.c` to confirm the
markers are still present, or diff against the last-known-good version.

### `Nano/Board/project_defs.h`

`ONE_SECOND` must match the actual timer tick rate:

```c
#define ONE_SECOND (31250)   /* PCLKD/1024 @ 32 MHz = 31.25 kHz → 32 µs/tick */
```

## Recovery procedure after `RASC → Generate Project Content`

1. `git diff` to see what was clobbered.
2. Restore `Nano/memory_regions.ld` from git (`git checkout Nano/memory_regions.ld`).
3. Re-apply the hal_data.c hand-edits in the table above — easiest with
   `git checkout Nano/ra_gen/hal_data.c` if your copy is committed.
4. `pio run -t clean && pio run && pio run -t upload`.
5. Confirm the yellow LED (P204) heartbeat (two quick flashes + long pause)
   within ~1 s of reset and verify the CLI prompt over SCI2 (P301/P302).

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
