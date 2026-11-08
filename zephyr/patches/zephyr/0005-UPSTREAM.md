# intc_renesas_rz_tint: clear only the interrupt's own status bit

Draft for an upstream Zephyr pull request. Patch:
`0005-intc_renesas_rz_tint-clear-only-own-status-bit.patch` (against v4.4.x).

## Problem

`drivers/interrupt_controller/intc_renesas_rz_tint.c`, when
`CONFIG_RENESAS_RZ_TINT_SUPPORT_STATUS_CLEAR_REG` is set (RZ/V2H, RZ/V2N), defines:

```c
#define TINT_STATUS_CLEAR(tint) REG_TSCLR_WRITE(tint, TINT_STATUS_READ(tint) | BIT(tint))
```

`TSCLR` is a write-1-to-clear register. `TINT_STATUS_READ()` returns the whole `TSCTR`
status word, so the write clears the status bit of every TINT slot that is pending at that
moment, not only the slot being serviced.

On a part where the TINT slots are shared with another core or OS, servicing one slot's
interrupt silently drops another slot's pending edge. The lost interrupt is never delivered.

## Fix

Write only the serviced slot's bit:

```c
#define TINT_STATUS_CLEAR(tint) REG_TSCLR_WRITE(tint, BIT(tint))
```

The other status-clear variant (`TSCR`, write-0-to-clear) is unchanged.

## How to reproduce

Needs an RZ/V2N or RZ/V2H board with two TINT slots in use (for example one owned by
Zephyr on the Cortex-M33 and one owned by Linux on the Cortex-A55), or two slots inside one
Zephyr image:

1. Configure two GPIO interrupts on different TINT slots, edge-triggered, both with a
   handler that counts calls.
2. Make both inputs produce an edge close together, so both status bits are set in `TSCTR`
   before the first ISR runs (drive both pins from one source, or pulse them back to back
   while the first ISR is delayed).
3. Observe: the handler for the first slot runs and clears; the second slot's status bit is
   cleared by the same `TSCLR` write and its handler never runs. With the fix both handlers
   run once per edge.

The loss is a race window, so repeat the stimulus (thousands of edges) and compare per-slot
handler counts with the edge count.

## Notes for the submitter

- Change is one line; no Kconfig or devicetree impact.
- Not reproducible on parts without `STATUS_CLEAR_REG` (they use the `TSCR` path).
- The dropped-status loss is derived from the source (TSCLR is write-1-to-clear) and has not
  been measured on silicon.
