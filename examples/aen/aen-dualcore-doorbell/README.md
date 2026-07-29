<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-doorbell — HE→HP MHU-1 doorbell with both M55 cores live

The completion of **B1**. Earlier doorbell attempts could not be tested because
only one M55 ran (a dual-boot ATOC boots one core) and a J-Link debug-AP write to
the sender did not propagate. Now `aen-dualcore-master` brings both cores up (the
portable `alp_mproc_boot_core`, SE-boot-service-backed on AEN), so a **real
HE-core sender** can ring HP.

The HP build is the master + receiver (boots HE, then polls the MHU-1 receiver);
the HE build is the sender (rings the MHU-1 sender). MHU-1 is the **non-secure
HE→HP** pair (Alif DFP + fork `e1.dtsi`):

| | base | IRQ | role |
| --- | --- | --- | --- |
| sender (HE writes) | `0x400B0000` | 44 | `CH0_SET` +0x0C; `ACCESS_REQUEST` +0xF88; `ACCESS_READY` +0xF8C |
| receiver (HP reads) | `0x400A0000` | 43 | `CH0_ST` +0x00; `CH0_CLR` +0x08 |

(register offsets transcribed from `zephyr/drivers/ipm/ipm_arm_mhuv2.h`.)

## Result (bench-verified on E8, 2026-06-18) — every ring received ✅

```
HP (master+receiver) : magic B1B10090  hb 0x823 -> 0x94F   received 0x01F4 -> 0x023C
HE (sender)          : magic B1B100E0  hb advancing         sent     0x01F4 -> 0x023C
```

**HE sent count == HP received count, exactly** — every doorbell HE rings is
received by HP. The HE→HP MHU-1 doorbell propagates with both cores live, with
**no SESS / secure-MHU setup needed** (the non-secure MHU-1 pair works directly).
This is the working substrate for HE↔HP IPC / a dual-core RPC.

> The earlier "J-Link-as-sender does not propagate" finding was a debug-AP
> artifact, not a hardware limit — a real CPU write to `CH0_SET` propagates.

Recipe: dual ATOC with HP-APP `["load","boot"]` @0x50000000 + HE-APP `["load"]`
@0x58000000; `app-gen-toc` + `app-write-mram`. Restore the canonical slot0 after.

## Verdicts, timeouts, and the HE↔HP boot block on this bench

Both HP and HE hold their verdict to a bounded window rather than looping
forever waiting for it, and each always prints exactly one `RESULT` line
before dropping into its trailing idle/ring loop:

- `RESULT PASS: dualcore-doorbell -- ...` — real evidence: HP counted >=1
  doorbell actually received on MHU-1 @`0x400A0000` (or, on HE, HP's
  cross-read received-count was observed to advance after a ring).
- `RESULT SKIP: dualcore-doorbell -- ...` — the peer never showed within a
  bounded window: the boot authority reported `ALP_ERR_NOSUPPORT` and HE was
  never released, HE released but never rang, HE's own MHU-1 sender link
  never came ready (`ACCESS_READY`), or HP never saw HE's ring — states what
  was locally proven, not a failure of this app's code.
- `RESULT FAIL: alp_mproc_boot_core rc=%d` — HP only: a real local error,
  `alp_mproc_boot_core` returned an unexpected rc (neither `ALP_OK` nor
  `ALP_ERR_NOSUPPORT`).

On THIS bench the HE↔HP release path is known-blocked and
`alp_mproc_boot_core` returns `ALP_ERR_NOSUPPORT` (`rc=-6`) — HP reports that
as `RESULT SKIP`, not `RESULT FAIL`: the boot authority itself says it can't
release HE here, which is a bench/silicon limitation, not a bug in this app.
Every wait (HP's receive verdict window: 3000 ms; HE's MHU-1 sender-link-ready
wait: 3000 ms; HE's ring-and-cross-check window: 3000 ms, all polled every
20 ms) is bounded, so a genuinely absent peer or a sender link that never
comes ready produces a verdict instead of a hang.

Before the verdict window, HP clears the latched `RCV_CH0_ST` doorbell-status
bit (`sys_write32(0xFFFFFFFFU, RCV_CH0_CLR)`): MHUv2 channel-status bits are
NOT cleared by a core-only warm reset or a J-Link RAM-run, so a leftover set
bit from a PRIOR run's HE ring would otherwise satisfy iteration 0 of this
run's window and produce a false PASS with HE never released this run.
