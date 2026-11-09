# ActiveRobot web page over USB

The Nano serves a web page that shows the robot's orientation and what every
muscle is doing. It travels over the same USB cable as the CLI, as a network
link (CDC-NCM): the board is `192.168.7.1`, the host is offered `192.168.7.2`
by the board's own DHCP server. No driver is needed on macOS, Linux or
Windows 11.

## Build, flash, open

It is an opt-in environment so the default build keeps its RAM and flash:

```
pio run -e nano_r4_web -t upload
```

Then browse to <http://192.168.7.1/> once the host's new network interface
shows "connected" (a few seconds after plug-in or reset). `ncm` on the CLI
shows link state and frame counters. `#demo` on the URL (`/#demo=rect4`,
`hex4`, `hex6`, `oct6`) runs the page on made-up data with no board involved,
which is also how to see tilt before an IMU is bound.

| env | adds | flash | RAM |
|---|---|---|---|
| `nano_r4` | | 107 KB | 17.0 KB |
| `nano_r4_web` | NCM function, lwIP, DHCP, HTTP, telnet, page, file transfer | 166 KB | 30.9 KB |

Two boards on one computer need different subnets or their interfaces clash:
build the second with `PLATFORMIO_BUILD_FLAGS="-D NCM_SUBNET=8" pio run -e nano_r4_web`
(the board is then 192.168.8.1; `ncm` shows the address). The default stays 7.

RAM is tight (about 1.9 KB free; measured peaks and what could still be trimmed are in `Docs/ram-budget.md`): lwIP, the NCM buffers and the HTTP
connections are all sized in `Board/lwip/lwipopts.h`, `Board/tusb_config.h`
and `platformio.ini` (`HTTP_MAX_CONNECTIONS`, `HTTP_REQ_BUF_SIZE`).

## Telnet and the file system

The web build also runs the telnet server (`Robot/net/telnet`): `telnet 192.168.7.1`
gives the same CLI as the UART, so the file words work over the USB link:
`ls`, `cat <name>`, `echo <name> <text>`, `cp`, `mv`, `rm`, `run <name>`. Cost:
about 1 KB of flash and 500 bytes of RAM (two more lwIP connections, a listener,
a 96-byte input queue and a 128-byte output buffer). There is no login; the link
is point-to-point to the USB host. A session is dropped after 5 minutes without
input. The file volume is small (6144 bytes, 32 files, names up to 15 characters).

## CLI transports (UART, USB serial, telnet)

All three feed the one CLI and share one output queue (`emitq`), so the rules in
`Robot/cli/cli_transport.[ch]` matter:

- The transport that last sent a byte owns the output. A transport's drain,
  including a TX-complete chain that keeps itself going from an interrupt, must
  do nothing unless `cli_transport_is_active()`. Before this was enforced, an idle
  UART chain pulled bytes out of a telnet session's output (duplicated and missing
  characters, and a `help` that was cut short).
- A transport that loses its peer (telnet session closed, USB terminal closed,
  cable pulled) calls `cli_transport_release()`: output goes back to the UART,
  the first transport to claim, and pending output is drained there instead of
  blocking the program.
- Input must reach the CLI from a scheduler action, never from inside lwIP's
  receive callback: `keyIn()` runs the command on the spot, and a command with a
  lot of output blocks and runs other actions (including the one that services
  lwIP) until the queue drains. Telnet queues its input for that reason.
- The output hook fires once per printed character. Telnet only schedules a
  flush from it; one action then writes in large pieces. One TCP segment per
  character overran the USB link layer (thousands of refused frames per `help`)
  and corrupted the stream.

Checked by comparing `help` (about 14 KB, 216 lines) byte for byte: 20 mixed
UART/telnet runs identical, USB serial identical when the first command follows
the port opening by 1.5 s or more. Sent within about half a second of opening
the port, a command's output can lose its first characters (the DTR line-state
handling lands in the middle of it); a person typing is not affected.

## File transfer

The page has a Files section; from a shell:

```
curl http://192.168.7.1/fs                          # JSON listing and space
curl -o calib http://192.168.7.1/fs/calib           # download
curl -T gait.cfg http://192.168.7.1/fs/gait.cfg     # upload (replaces)
curl -X DELETE http://192.168.7.1/fs/gait.cfg       # remove
```

Names are 1-15 characters of letters, digits, `.`, `_`, `-`. One transfer at a
time (a second gets 503). An upload is written to the end of the log as it
arrives and the directory changes only when the last byte is in, so a dropped
connection or a power loss leaves the old file intact; a stalled upload is
dropped after 10-15 s. Because of that, replacing a file needs room for old and
new together (507 otherwise: delete the old one first). While an upload is open
the CLI's own file changes (`echo`, `rm`...) answer "busy". When the new file fits only after the
log is compacted (space held by deleted or replaced files), the upload answers `503` with
`Retry-After: 1` and the board compacts in the main loop; ask again (the page retries 5 times,
`curl --retry 5` does the same). A file that cannot fit even compacted gets 507 at once. The routes are in
`Robot/net/web/robot_files.c`, built on `Robot/net/http`'s stream routes and
`fs_write_*` in `Robot/storage/fs`; host tests: `Robot/storage/fs/test/`.

## Robot model

`body` shows it, `body! ( code -- )` sets it; `calib-save` keeps it.

| code | model | body | limbs |
|---|---|---|---|
| 0 | `rect4` | rectangle | 1, 2, 5, 6 |
| 1 | `hex4` | hexagon | 1, 2, 5, 6 |
| 2 | `hex6` | hexagon | 1-6 |
| 3 | `oct6` | octagon | 1-6 |

Limbs are numbered as in the muscle module: rows front 1/2, middle 3/4, rear
5/6, odd on the left (LF, RF, LM, RM, LR, RR). On the hexagon models a limb
points at +30 / -30 (front), +90 / -90 (middle), +150 / -150 (rear) degrees
from forward, positive to the left; the hexagon's vertices sit on those axes.
`oct6` is the as-built `hex_ctrl` board (`board-NanoR4/PCB`), which moved
from a hexagon to an octagon so every limb edge lands on a 0/45/90 multiple
the autorouter can route (see `PCB/README.md`); its front/rear angles tighten
to +45 / -45 and +135 / -135, landing on the octagon's edges instead, with
the middle limbs still at +90 / -90. Rear limbs are mounted mirror-image, so
their forward/back muscles swing the foot the other way (`mirror` in
`/model`, as in `gait.c`). The middle limbs are assumed to be mounted like
the front pair.

The model only describes the body for the page. It does not change which
muscles the muscle module treats as fitted (`MUSCLE_LIMBS_4` in `hal_entry.c`).

## Endpoints

| path | what |
|---|---|
| `/` | the page |
| `/model` | JSON: model code, name, shape, limb count, each limb's side, row, angle, mirror |
| `/events` | server-sent events, 10 per second while a browser is connected |
| `/fs`, `/fs/<name>` | file listing, download, upload, delete (see File transfer) |
| `/term_stream`, `/term_in` | the CLI over HTTP (built into `Robot/net/http`) |

An `/events` message: `{"r":-12,"p":34,"v":1,"t":0,"g":2,"m":[...24...]}`
with roll and pitch in tenths of a degree (x forward, y left, z up; roll > 0
left side up, pitch > 0 nose down), `v` 1 once the IMU estimate is valid, `t`
1 if tipped, `g` the model code (the page re-reads `/model` when it changes),
and `m` the 24 muscle activations in percent, muscle 1 first. Roll, pitch and
`v` stay 0 until `posture_bind_hw()` is called (no IMU is bound yet).

## Where the code is

| | |
|---|---|
| `Robot/limbs/model/` | body model, `body` words (all builds) |
| `Robot/net/web/robot_web.[ch]` | `/model`, `/events`, routes |
| `Robot/net/web/robot_page.html` | the page (edit this) |
| `Robot/net/web/gen_page.py` | page to C string, run by `Scripts/gen_robot_page.py` before each web build |
| `Board/usb_ncm.[ch]` | NCM <-> lwIP glue, DHCP, `ncm` word |
| `Board/lwip/` | lwIP port options for the Nano |
| `Board/usb_init.c`, `Board/tusb_config.h` | NCM interface in the USB descriptors |
| `Board/cli_transport_cdc.c` | CLI over USB CDC (moved here from `Robot/cli/`: it uses `tusb.h`) |

## Things that cost time

- **macOS drops the NCM link on about a third of attaches** (it sets alt 1 then
  alt 0 within 5 ms and never retries; Apple knows, see Apple Developer Forums
  thread 824727). Apple's workaround is `bmNetworkCapabilities = 0x01` in the NCM
  functional descriptor: the `CFG_TUD_NCM_NETWORK_CAPABILITIES` macro in
  `tinyusb/src/device/usbd.h` (a board patch), set in `Board/tusb_config.h`.
  With it: 50 of 50 reboots linked.
- **Shared object directories.** PlatformIO builds sources outside the project
  (`../tinyusb`, `../Robot`, `../../TimbreOS`) into `.pio/build/<name>/`, shared by
  every environment while each environment tracks them separately. Without
  `Scripts/per_env_objects.py` (clears them when the active environment changes)
  the web firmware could link TinyUSB compiled without the NCM class: the host
  then sees the device addressed but never configured. Suspect this first for
  "worked, then didn't after I built another env".
- Right after a flash the Mac can take 5-15 s to bring the interface up, and an
  early request can leave a stale "incomplete" ARP entry. Give it a moment.
