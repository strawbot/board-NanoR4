# TimbreOS / ActiveRobot Nano R4 — Session Handoff

## Hardware
- **Board**: Arduino Nano R4 (genuine Arduino, VID 0x2341, PID 0x0074)
- **MCU**: Renesas RA4M1 (Cortex-M4, 48MHz, 256KB flash, 32KB RAM)
- **Debugger**: Segger J-Link available but not yet connected (requires soldering to 5 SWD pads on underside of board)
- **Console**: UART via USB-to-TTL adapter on hardware UART pins (not USB CDC)
- **Programming**: DFU bootloader via USB (double-tap reset to enter), PlatformIO upload works

## Project Structure
Three peer folders on the user's Mac:
```
<parent>/
├── Nano/          ← PlatformIO + RASC unified project (THIS workspace)
├── NanoR4/        ← original RASC-only project (can be retired)
└── TimbreOS/      ← shared OS library (NOT yet accessible in Cowork)
```
The `TimbreOS/` folder cannot be added to the Cowork session due to a persistent
"Session VM process not available" error. This is the main unresolved issue.
The fix is to select the **parent folder** as the Cowork workspace in a fresh session.

## Development Environment
- **IDE**: VS Code with PlatformIO extension + Renesas VS Code extension (RASC)
- **Build**: PlatformIO, framework = fsp (bare metal Renesas FSP, NOT Arduino)
- **RASC**: configuration.xml lives in Nano/ root; generates ra/, ra_cfg/, ra_gen/
- **Workspace file**: Nano/Nano-R4.code-workspace

## platformio.ini (current state)
```ini
[env:nano_r4]
platform = renesas-ra
board = nano_r4
framework = fsp
monitor_speed = 115200
monitor_port = /dev/cu.usbmodem113201
lib_extra_dirs = ../TimbreOS
build_src_filter = +<*> +<../Board/**/*> -<../Board/gpio_dump.c> -<../Board/mocks.c>
build_flags = -I Board -I Board/include
; Pre-build scripts — uncomment when ready:
; extra_scripts =
;     pre:Scripts/run_pin_gen.py      ; needs rewrite for RASC pin format (was STM32 CubeMX)
;     pre:Scripts/run_parsewords.py   ; enable once TimbreOS folder is mounted
```

## OS Concept: TEA (Time / Event / Action)
- TimbreOS is a bare metal OS based on TEA architecture
- No Arduino framework — FSP only
- UART console (not USB CDC) with DMA ring buffer input
- Entry point: `src/hal_entry.c` (FSP convention, called from ra_gen/main.c)

## RASC Configuration (configuration.xml in Nano/)
Stacks configured so far:
- **r_sci_uart** (SCI2) — UART console, FIFO level 1, 15-bit idle timeout callback
- **r_dmac** — receive path, SCI2 RXI activation, repeat mode, 128 transfers/block,
  2 blocks (256-byte ring buffer), interrupt after each block, callback: cli_rx
- **r_dtc** — transmit path, locked by r_sci_uart (correct behaviour)
- **r_gpt (32-bit)** — tick timer, channels 0 or 1, 100µs period (10KHz)
- **r_gpt (16-bit)** — second timer, channels 2-7
- **r_elc** — ELC driver (required for GPT3 overflow → tick_timer cascade)
- **r_ioport** — GPIO, part of BSP, auto-initialised

### DMA ring buffer design
- 256-byte circular buffer, split into two 128-byte blocks
- Two triggers to process incoming data:
  1. DMA block interrupt (half-full at 128 bytes)
  2. SCI UART FIFO idle timeout (15 bit-times after last character = end of message)

### Timer design
- GPT0 or GPT1 (32-bit): 100µs system tick at 48MHz PCLKD
  - prescaler /8 → 6MHz timer clock → period count 600 → 100µs
- ONE_SECOND defined as 10000 ticks (100µs resolution)
- Timer cascading via ELC: GPT3 overflow → tick_timer count input
- DWT->CYCCNT used for hi-res timing (needs enabling at boot)

## Board Folder (Nano/Board/) — Porting Status
Copied from a peer STM32 project, being ported to RA4M1/FSP.

| File | Status |
|------|--------|
| project_defs.h | Needs minor changes (see below) |
| clocks.h/c | Not yet assessed |
| cli_transport_usart.h/c | Not yet assessed |
| board_cli.h/c | Not yet assessed |
| gpio_dump.c | **Excluded from build** — needs full rewrite for RA4M1 IOPORT |
| mocks.c | **Excluded from build** — needs assessment |
| boardwords.txt | Input for parsewords.py (not yet run) |

### project_defs.h changes needed
1. Replace `#include "stm32wb55xx.h"` with `#include "bsp_api.h"`
2. Change `CLOCK_MHZ 32u` to `CLOCK_MHZ 48u`
3. Enable DWT cycle counter at boot in hal_entry.c:
   ```c
   CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
   DWT->CYCCNT = 0;
   DWT->CTRL   |= DWT_CTRL_CYCCNTENA_Msk;
   ```
4. Everything else (CMSIS intrinsics, SCB->ICSR, ENTER/LEAVE_REGION) is portable

## Scripts (Nano/Scripts/)
- **get_pin_names.py** — parses STM32CubeMX main.h to update alias_table[] in gpio_dump.c.
  Needs rewriting to parse RASC-generated pin files instead (ra_gen/pin_data.c or bsp_pin_cfg.h)
- **run_pin_gen.py** — pre-build wrapper (not yet created, blocked on above rewrite)

## TimbreOS Folder (../TimbreOS/) — Not Yet Accessible
- Contains **parsewords.py** — generates source code from word definition files
- Nano/Board/boardwords.txt is the input file for parsewords.py
- pre:Scripts/run_parsewords.py wrapper not yet written (blocked on access)

## GPIO / LED
- Built-in LED: P102 (BSP_IO_PORT_01_PIN_02), same as UNO R4 Minima
- FSP API: `R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_02, BSP_IO_LEVEL_HIGH)`
- g_ioport_ctrl declared in ra_gen/common_data.c, available via hal_data.h

## J-Link Debug (future)
When SWD pads are soldered, add to platformio.ini:
```ini
debug_tool = jlink
upload_protocol = jlink
```

## Next Steps (priority order)
1. **Fix Cowork folder access** — start new session selecting parent folder of Nano/NanoR4/TimbreOS
2. **Read parsewords.py** — understand what it generates so run_parsewords.py wrapper can be written
3. **Port project_defs.h** — minor changes listed above
4. **Assess clocks.c, cli_transport_usart.c, board_cli.c** — determine porting scope
5. **Attempt first FSP build** — see what errors remain
6. **Rewrite get_pin_names.py** — parse RASC pin output instead of CubeMX main.h
7. **Port gpio_dump.c** — rewrite for RA4M1 IOPORT registers
