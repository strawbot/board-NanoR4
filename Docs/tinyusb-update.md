# Moving to stock TinyUSB (0.20.0 + 3 patches  ->  upstream master)

Until now `../tinyusb` was TinyUSB 0.20.0 (Nov 2025) plus three local commits
that existed only in that clone. This branch moves to **unmodified upstream** so
the submodule can point at a public commit and nothing of ours lives in the
TinyUSB tree.

Tested commit: **`f652ab900`** (hathach/tinyusb master, 2026-10-30, "0.21.0 + 776").
The 0.21.0 tag itself (2026-06-30) is too early: it lacks the NCM carrier-lost
fix, the RUSB2 SQCLR/EP0 fixes and the OUT-endpoint RX_PENDING work.

## What the three local patches were, and why each can go

| Patch | Purpose | Now |
|---|---|---|
| `rusb2` bounded spins | every register-readback loop in `dcd_rusb2.c` was unbounded and ran in the ISR | upstream bounds the FIFO-ready waits itself (7b1eb4f86, 24f8bce0b) |
| `rusb2` `process_set_address` one-shot guard + `DCPCTR` re-arm | "belt and suspenders" from a wrong theory; the real fix for the lost interrupt is the re-check loop in `usbfs_int_isr()` (`Board/usb_init.c`) | dropped; tested without it (below) |
| `usbd.h` NCM `bmNetworkCapabilities` macro | Apple's workaround for the macOS SET_INTERFACE alt1/alt0 bug (forum thread 824727) | upstream's `TUD_CDC_NCM_DESCRIPTOR` has a `_capability` argument and implements `SetEthernetPacketFilter`; we pass `NCM_NETWORK_CAPS_ETH_FILTER` |
| usb_trace checkpoints | retired | already removed |

The old diff is kept in `tinyusb-0.20.0-board-patches.patch` (against 0.20.0)
and the old tree is tagged `activerobot-0.20.0-patched` in the local tinyusb clone.

## Changes on our side (the whole port)

* `Board/usb_init.c`: NCM descriptor uses the new 11-argument macro
  (`CFG_TUD_NET_ENDPOINT_SIZE` no longer exists: endpoint size 64, notification
  interval 50 ms as before, capabilities `NCM_NETWORK_CAPS_ETH_FILTER`).
* `Board/tusb_config.h`: the `CFG_TUD_NCM_NETWORK_CAPABILITIES` macro is gone.

Nothing else needed changing: all three environments build.

## To adopt

The descriptor change does not compile against the old patched tree and the old
tree does not accept the new one, so move both together:

```bash
git -C ../tinyusb checkout f652ab900        # stock; or a newer upstream commit you have re-tested
git merge tinyusb-0.21                      # in board-NanoR4
```

Other boards' sessions share `../tinyusb`, so do this when none of them is
mid-build. Rollback: `git -C ../tinyusb checkout activerobot-0.20.0-patched` and
revert the merge.

## Size (build of the same sources, 2026-10-31)

| env | RAM before | RAM after | flash before | flash after |
|---|---|---|---|---|
| nano_r4 | 19,148 | 19,040 | 120,640 | 121,216 |
| nano_r4_web | 30,892 | 30,800 | 166,880 | 168,172 |
| nano_r4_usbtest | 20,180 | 20,180 | n/a (no TinyUSB) | n/a |

## Test log (board 7DB on the J-Link, web build, NCM on 192.168.8.1)

* J-Link reset loop: 130 / 130 came back with web page and CDC answering,
  4.5 - 4.6 s each.
* DFU bootloader -> flash -> app handoff: 15 / 15.
* Parallel floods (`/model`, downloads, uploads that compact, events, UART `help`):
  3 mixes, no hang, stack peak 1,244 of 2,048 bytes; `help` identical on UART and CDC.
* Cold unplug / replug of the USB cable: all attempts came back (final state checked).
* Idle soak (page polled every 30 s, no gap): board 7DB ran 10 h+ (02:20 - 12:25 on
  2026-10-31) and board 82C 8 h+ (from 03:46) without a stop. 82C had hung at idle
  three times on the patched 0.20.0 tree (<= 10 h, <= 7 h, 178 min; the last two
  already had the accept fix), so this supports the update curing it. It is not
  proof: treat a recurrence as a new bug and halt the board with a debugger.

## Notes

* A board whose bootloader enumerates as `2341:0369` (7DB) cannot be flashed by
  `pio run -t upload` from `env:nano_r4_web`: PlatformIO's dfu filter only accepts the
  stock Nano R4's `0074/0374`, so it reports "No DFU capable USB device" and leaves the
  board in its bootloader. Use `dfu-util -d 0x2341:0x0369 -p <usb path> -a 0 -D
  .pio/build/nano_r4_web/firmware.bin` (or give the web env a `nano_r4_dbg` board).
