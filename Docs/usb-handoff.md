# USB device stack — handoff

**Status: disabled.** `USB_TRANSPORT_ENABLE` is `0` in `Board/usb_init.c`.
Starting the TinyUSB device stack takes the board down. Everything else works.

This note exists because the USB problem was found at the end of a long hunt
for something that looked nothing like a USB problem, and the context is worth
keeping.

---

## What is still true with USB off

`usb_transport_init()` still calls `usb_hw_reset()`. That is not optional — it
is what makes the board boot reliably after a DFU download. USBFS is left
detached, all interrupt sources masked and cleared, NVIC entry disabled.

The `dfu` CLI word still works. It arms `VBTBKR[0]` and resets; no USB stack is
involved. So the route into the bootloader stays open.

---

## The symptom, and why it was so misleading

A reset button press after a DFU session left the board dead — no console, no
LED, no fault. A power cycle always worked, and a second reset press always
worked. That asymmetry sent the investigation through the bootloader, the VBATT
domain, the reset circuit and the print path before landing on USB.

The chain of causation, once assembled:

1. `tusb_init()` enables `USBFS_INT`, but `tud_task()` is what services it and
   that only runs from `run()`.
2. Coming out of DFU mode, USBFS is already live and the host still has the
   bootloader's DFU device attached, because `boot5()` stops the module without
   ever clearing `SYSCFG.DPRPU` — it never detaches from the bus.
3. So an interrupt condition arrives with nothing to clear it, and the ISR
   re-enters continuously.
4. That starves the SCI TX interrupt, so `emitq` stops draining.
5. `output()` busy-waits for space in `emitq` **with interrupts masked**, which
   is a deadlock: the only thing that can free space is the ISR that can no
   longer run.

Step 5 is why it presented as a printing bug, and why the board went completely
silent — SysTick could not fire either, so even a watchdog stayed quiet.

## Measurements that pinned it down

- 380 characters of flood output before `usb_transport_init()`: fine.
- The same flood immediately after it: dies within ~15-20 characters, and
  progressively sooner on each attempt.
- Moving `usb_transport_init()` to just before the run loop got the whole boot
  to print, prompt and all — then it hung on the first `run()`, which is the
  first call to `tud_task()`.
- An early `return` after `usb_hw_reset()`: boots reliably after a DFU download.

---

## When picking this back up

1. **Set `USB_TRANSPORT_ENABLE` to 1** in `Board/usb_init.c`.
2. **Instrument the ISR first.** Put a counter in `usbfs_int_isr()` and expose
   it as a CLI word. If it climbs into the millions in a second, the storm is
   confirmed and `INTSTS0` (`0x4009_0040`) names the condition.
3. **Suspect the unhandled source.** TinyUSB's RA port clears what it knows
   about. Anything enabled in `INTENB0` that `dcd_rusb2` does not handle will
   re-enter forever. Compare `INTENB0` against what the ISR actually clears.
4. **Check `dcd_int_enable()`.** `usb_transport_init()` never calls
   `board_init_after_tusb()`, which is where TinyUSB's own BSP normally enables
   the USB interrupt. It may be getting enabled somewhere it should not be, or
   at the wrong point.
5. **Keep the ordering.** USB comes up immediately before the run loop
   (`hal_entry.c`). Do not move it earlier — nothing can service it until
   `run()` is turning.

## The bug that should be fixed regardless

`output()` must not busy-wait for the ISR while holding interrupts masked. That
is unsafe anywhere, not just at boot — any burst that outruns the UART inside a
masked region can deadlock the board, and it will look like a hang with no
diagnostic trail at all.

`Board/cli_transport_usart.c` has `usart_drain()`, which paces the producer so
`emitq` never fills. That is containment. The cure belongs in `output()`, and
since it is shared across every board, in `Robot/`.

Related, already fixed: `usart_emit()` claimed the TX shuttle with a
non-atomic test-and-set while being callable from both thread and ISR context.
It is now inside a critical section.

---

## Diagnostics left in place

Worth keeping — they cost nothing and turn a silent `while(1)` into something
readable.

| What | Where | How to read it |
| --- | --- | --- |
| Fault handler | `Board/fault.c` | 20 LED pulses; `hex 4001E504 2 dump` for marker `0xFA017ED0`, PC, LR, xPSR, CFSR, HFSR |
| Boot watchdog | `Board/fault.c` | 15 LED pulses; `hex 4001E520 2 dump` for marker `0x0B007D06`, PC, LR. Armed in `hal_entry`, disarmed before the run loop |
| Boot breadcrumb | `mark()` in `hal_entry.c` | `hex 4001E51C 1 dump` — last boot step reached |
| LED pulse marks | `BOOT_MARKS` in `hal_entry.c` | off by default, adds ~2 s to boot |
| Flood test | `BOOT_FLOOD_TEST` in `hal_entry.c` | off by default; regression check for the `emitq` deadlock |

All the VBATT stashes depend on `VBTBER.VBAE` being set first — without it the
backup registers accept writes and read back correctly but retain nothing.

---

## Reference — registers this work depends on

| Register | Address | Notes |
| --- | --- | --- |
| `PRCR` | `0x4001_E3FE` | PRC1 unlock `0xA502`, lock `0xA500` |
| `VBTBER` | `0x4001_E4C0` | bit 3 `VBAE` — must be 1 for `VBTBKR` retention |
| `VBTBKR[0]` | `0x4001_E500` | bootloader double-tap magic `0x0773_8135` |
| `USBFS SYSCFG` | `0x4009_0000` | bit 0 `USBE`, bit 4 `DPRPU` |
| `USBFS INTENB0` | `0x4009_0030` | |
| `USBFS INTSTS0` | `0x4009_0040` | bits 6:4 `DVSQ` device state |
| `USBFS_INT` | IRQ 6 | `ra_gen/vector_data.c` |

Bootloader landmarks are in `Docs/board-a-vs-b-reset-probes.md`, and the
disassembled 16 KB image is at `Docs/nano_r4_bootloader_16k.bin`.
