# output() under backpressure — analysis and options

Design note for the shared `output()` in TimbreOS. Written from the Nano R4
side, but the problem is board-agnostic.

## Correction to the record

Earlier notes in this project claimed `output()` masks interrupts while
blocking. **That was wrong** — an inference from SysTick failing to fire during
a stall, stated as if it were established. `output()` bleeds characters and runs
actions while blocked, which is a reasonable design and not the bug.

Why SysTick did not fire during the stall is still unexplained. Candidates worth
a few minutes if it matters: the weak `SysTick_Handler` override not resolving,
`SystemCoreClock` disagreeing with the actual ICLK, or the interrupt being
starved by a higher-priority source. Not chased, because the USB finding made it
moot.

## The actual defect: a lost wakeup

The transmit chain is self-sustaining only while bytes keep completing:

```
usart_emit()  ->  R_SCI_UART_Write(1 byte)  ->  UART_EVENT_TX_COMPLETE
                                             ->  cli_rxtx() -> usart_emit()
```

When `emitq` runs empty, `usart_emit()` returns early and **the chain stops**.
Restarting it requires an explicit call, which arrives only via
`when(EmitEvent, usart_emit)`.

Now consider `output()` blocked on a full queue. It runs actions while it waits
— but running actions does not signal `EmitEvent`. If the chain went idle at the
wrong moment, nothing will ever call `usart_emit()` again. `output()` spins
forever, faithfully executing the fallback that cannot possibly help.

This is why `usart_drain()` fixed the flood test even before USB entered the
picture: it calls `usart_emit()` directly on every pass.

There was also a genuine race in `usart_emit()` — a non-atomic test-and-set of
`tx_busy` while callable from both thread and ISR context, which could leave
`tx_busy` stuck true with no write in flight. Fixed; see
`Board/cli_transport_usart.c`.

## The real question

You framed it correctly: when a producer outruns the serial bandwidth, the only
choices are block the producer or discard characters. Both are legitimate; the
mistake is having no explicit policy, because then the behaviour is decided by
whichever failure mode happens to trigger.

Worth separating two properties:

- **Liveness** — the system must always make progress. Non-negotiable.
- **Completeness** — no character is lost. Negotiable, and should differ by
  stream.

The present design chose completeness and blocking, then lost liveness to a
missing wakeup. Fixing liveness is the priority; the policy question can follow.

---

## Option 1 — Kick the transport while blocked

The minimal fix, and the one that removes the entire lost-wakeup class.

While `output()` waits for space, it should call the transport's emit function,
not merely run actions. Give the transport a registered kick:

```c
static void (*emit_kick)(void);          /* set by the transport at init */

/* inside output()'s wait loop */
if (emit_kick) emit_kick();
runOneAction();
```

`usart_emit()` is already idempotent and now atomic, so calling it
unconditionally is safe. Cost is one indirect call per spin.

This alone would have prevented the boot hang, and it makes `usart_drain()`
redundant.

## Option 2 — Bounded wait with a guaranteed-progress fallback

Liveness should not depend on an interrupt arriving. Track queue depth; if it
hasn't changed in N milliseconds, stop trusting the ISR path and push a byte
directly:

```c
/* transport-provided, polled, no interrupts required */
void usart_emit_blocking_byte(uint8_t b);
```

Slow, but it cannot deadlock. For a debug console this matters more than
throughput — the console is most needed exactly when the rest of the system is
broken. A console that dies in the failure it exists to report is the worst
possible failure mode, and that is precisely what happened here.

## Option 3 — Re-entrancy guard

`output()` running actions while blocked means an action can call `print()`,
which can block, which runs actions, which can call `print()`. That recursion is
bounded only by luck and the stack.

A depth counter costs almost nothing:

```c
static uint8_t output_depth;

if (output_depth) {
    /* nested: kick and spin, but do NOT dispatch actions again */
} else {
    output_depth++;  ... runOneAction(); ...  output_depth--;
}
```

Worth adding regardless of the rest — the `stack_canary_init()` machinery
already in the build suggests stack depth is a live concern.

## Option 4 — Yield instead of block

The structurally correct answer for a cooperative scheduler: a producer that
can't finish should return to the scheduler and resume later, not block inside
`output()`.

For a Forth-like CLI this is tractable at line granularity — `dump` emits one
line per pass and re-queues itself, so a 16 KB dump becomes 1024 scheduler
passes and never blocks anything. It needs each bulk-emitting word to be
resumable, so it's real work, but it's the version with no backpressure
pathology at all.

Worth doing for the handful of words that generate bulk output (`dump`, `words`,
`help`). Not worth it for ordinary short replies.

## Option 5 — Per-stream policy

Not all output deserves the same treatment:

| Stream | Policy |
| --- | --- |
| CLI replies | Block. The user asked; losing characters is a wrong answer. |
| Boot diagnostics | Block, but bounded — Option 2's fallback. |
| Telemetry / periodic | Drop, and count. |

For dropped streams, keep a counter and emit `[N dropped]` when the queue
recovers. Silent loss is much worse than acknowledged loss — it looks like a
bug in whatever produced the output.

## Option 6 — Cheap supporting changes

- **Queue high-water mark**, exposed as a CLI word. Tells you how close to the
  edge normal operation runs. Boot output on this board was apparently sitting
  just under the limit, which is why the failure looked intermittent and
  timing-dependent.
- **Raise the SCI TX interrupt priority** above chatty peripherals. The USB
  starvation that broke this board would have been survivable if the drain
  couldn't be preempted by a storming source.
- **Larger `emitq`.** Doesn't fix anything, but pushes the boot case away from
  the edge while the real fixes land.

---

## Recommendation

In order:

1. **Option 1** — kick the transport while blocked. Small, removes the bug class
   that caused this, and lets `usart_drain()` be deleted.
2. **Option 3** — re-entrancy guard. A few lines, prevents a nastier failure
   that hasn't bitten yet.
3. **Option 2** — bounded wait with polled fallback. Makes the console
   structurally trustworthy under failure.
4. **Option 6** — high-water mark and TX interrupt priority. Cheap visibility
   and margin.
5. **Option 4** for bulk words, when convenient.
6. **Option 5** when a telemetry stream actually exists.

Per `Nano/CLAUDE.md`, all of this belongs in `Robot/` or TimbreOS — it is
board-agnostic. The transport-specific pieces (the kick function pointer, the
polled byte writer) are the hooks each board supplies.
