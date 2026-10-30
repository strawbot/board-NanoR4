# RAM budget of the web build (measured 2026-10-30)

`nano_r4_web` is the tight one: 32,768 bytes of RAM, of which **30,880 are used (94.2%)**
after the savings below (it was 32,336, 98.7%). `nano_r4` uses 19,144.

Measured with a J-Link on the 7DB board, reading RAM without halting (sampler: heap blocks,
memory-pool free lists, timer table, stack canary at ~50 Hz) while a heavy load ran: the event
stream held open, parallel page/model/file requests, 3 KB uploads and downloads, telnet
sessions running `help te telist mstats`, and USB-serial sessions.

## What is in RAM

| bytes | what | notes |
|---:|---|---|
| 27,516 | `.bss` (zero-initialised) | |
| 2,680 | `.data` (initialised) | nothing constant in it: the queues (`keyq`, `emitq`, `eventq`, `actionq`) and stacks of the CLI, small mutable FSP structs. No `const` candidates. |
| 2,072 | stack area (`g_main_stack`, 2 KB) | no heap is reserved |

Largest items, web build: `ncm_epbuf` 2,616 (USB network transfer buffers), `conns` 2,412 (3 HTTP
connections x 768-byte request buffers), `ram_heap` 2,067 (lwIP heap), `g_main_stack` 2,048,
`calib pending` 1,920, `PBUF_POOL` 1,827, `muscle_sr sch` 1,332, `tea tes` 1,280, `fs g_buf` 1,024,
`hereSpace` 1,000, the four `tea` name/time tables 4 x 508.

## Measured peaks under load

| resource | size | peak | verdict |
|---|---:|---:|---|
| stack | 2,048 | 1,280 - 1,944 (varies run to run) | **do not shrink**; margin is thin, see below |
| lwIP heap | 2,048 | 1,880 - 2,048 | **saturated under load; do not shrink** |
| lwIP TCP_PCB pool | 6 | 6 | exhausted under a parallel flood: a telnet connect can fail until some HTTP connections close |
| lwIP PBUF_POOL | 3 | 2 | no room to trim |
| lwIP TCP_SEG | 8 | 7 | |
| tea time events (`NUM_TE`) | 80 | 6 - 7 | 80 is far more than used |
| named actions (`tea` tables) | 63 usable | ~28 | a 31-slot table would be nearly full |
| HTTP request header (Chrome-class) | 768 | 444 (GET), 372 (PUT) | 768 is comfortable; Chrome with all its client hints is ~600 |
| CLI dictionary (`hereSpace`) | 1,000 | 410 | spare capacity for user-defined words |

## Done (1,456 bytes)

- `strtoul` in the HTTP server replaced with a four-line parser: it pulled in newlib's reentrancy
  block and stdio structures, ~600 bytes of RAM, for one number.
- `/model` and the download header are generated a piece at a time (`Robot/net/web/web_slice.h`)
  instead of built into 516- and 200-byte buffers.
- lwIP `ARP_TABLE_SIZE` 10 -> 4 (a point-to-point link).

## Not done: candidates, by size

| bytes | change | risk / who |
|---:|---|---|
| ~900 | `calib pending` (48 entries x 40 B): 32 entries and 6 values, or stream the lines | the calibration code is shared; entries are 5 values per wire at most |
| ~770 | `NUM_TE` 80 -> 32 in `Board/project_defs.h` | peak is 6-7; low risk |
| ~1,360 + ~350 | wirecal and analog-test working buffers are only used while those tools run: share one arena with other rarely-used buffers (e.g. `fs g_buf`) | needs a rule that they never run together |
| ~770 | HTTP `HTTP_REQ_BUF_SIZE` 768 -> 512 | tight for Chrome PUT/DELETE with client hints |
| ~500 | `CFG_TUD_NCM_OUT_NTB_MAX_SIZE` 1200 -> 700 | the NCM spec asks for 2048; Linux/Windows may object, only macOS tested |
| ~590 | `PBUF_POOL_SIZE` 3 -> 2 | peak in use is already 2 |
| 1,016 | `tea` hash tables 127 -> 63 slots | only 31 usable, ~28 named actions exist |

Not worth it: the stack and the lwIP heap (both at their limits under load), `hereSpace` (user
capacity), `.data` (nothing constant).

## What the debugger found while measuring

- **`in()` runs its action inside the timer interrupt; `after()` queues it for the main loop.**
  lwIP's timer service (`ncm_service`), the event stream's pushes (`events_tick`) and telnet's
  retry were using `in()`, so lwIP's non-reentrant raw API was being entered from an interrupt while
  the main loop was in the middle of lwIP code. The watchpoint showed it directly: a stack 1,728
  bytes deep with `tcp_slowtmr -> tcp_output -> inet_chksum_pseudo` running in the timer interrupt on
  top of an HTTP upload, itself inside a flash erase. All three now use `after()`. Rule: anything
  that calls lwIP, the file system or `printf`-style output must run from the scheduler, never
  from `in()`. (`calib_step`, wirecal and the ADC trackers also use `in()`; `calib_step` does file
  system work at interrupt level and should move to `after()`.)
- **Stack depth is dominated by one path:** `usb_action -> tud_task -> NCM receive -> lwIP -> http_recv
  -> put_begin -> fs_write_begin -> compact -> flash erase`, with a timer interrupt on top. Doing the
  compaction from an action instead of from inside the network callback would cut the worst case by
  several hundred bytes.
- A GDB data watchpoint must be disarmed before leaving the chip: it stays armed in the DWT after
  the session ends, and with no debugger attached the match raises a fault.
