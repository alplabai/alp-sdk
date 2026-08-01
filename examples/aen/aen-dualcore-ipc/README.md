<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-ipc — bidirectional HE↔HP shared-memory request/response

Real data IPC between the two M55 cores, built on the proven dual-core boot
(the portable `alp_mproc_boot_core`, SE-boot-service-backed on AEN) + MHU-1
doorbell. HE (requester) writes a `{seq, len,
payload}` message into a shared global-SRAM0 mailbox and rings HP; HP (responder)
replies with `payload+1` into a reply mailbox and rings HE; HE verifies the reply
and counts the round-trip. Board-aware single app (HP build = master + responder,
HE build = requester).

## The reverse ring: one MHU pair, both directions

The non-secure **MHU-1 base is a CPU-relative alias** (Alif DFP, `rtss_he/soc.h`
+ `rtss_hp/soc.h`): from *either* core, `0x400B0000` = "my TX to the other core"
and `0x400A0000` = "my RX from the other core" — the fabric cross-routes each
core's TX frame into the other's RX. So **one** pair carries both directions; no
secure MHU-0 / SESS. (The HE→HP half is proven in `aen-dualcore-doorbell`.)

## Two correctness requirements (both bench-found)

1. **Handshake on the shared `seq`, not the doorbell edge.** The single-bit MHU
   channel races on back-to-back rings (stalled after 1 round-trip). The reliable
   "message ready" signal is the mailbox `seq` (coherent SRAM); the doorbell is a
   non-blocking latency hint, drained but never blocked on.
2. **`CONFIG_DCACHE=n`.** The mailbox is read/written by both cores, each with its
   own D-cache → cross-core reads saw stale lines (HE never observed HP's reply).
   Disabling the D-cache makes the shared region coherent (the AEN-SRAM
   precedent). A cache-on variant would `sys_cache_data_flush_range` on the writer
   + `invd_range` on the reader. `seq` is written LAST (after a DMB) so a consumer
   never reads a half-written message.

## Result (bench-verified on E8, 2026-06-18) — 64/64 round-trips ✅

```
RT_DONE   @0x02002080 = 0x40 (64 round-trips completed)
RT_BAD    @0x02002084 = 0    (every reply == request payload + 1)
HP_SERVED @0x02002088 = 0x40 (64 requests serviced)
```

Every one of 64 request/response round-trips completed with the payload verified
correct. This is a working bidirectional IPC channel between the M55 cores — the
substrate for a hand-rolled RPC (the OpenAMP/RPMsg path is the heavier
alternative; see `aen-rpc-pingpong`).

Recipe: dual ATOC HP `["load","boot"]` @0x50000000 + HE `["load"]` @0x58000000;
`app-gen-toc` + `app-write-mram`; read the beacons over SWD; restore canonical
slot0 after.

## Verdicts, timeouts, and the HE↔HP boot block on this bench

Both HP and HE hold their verdict to a bounded window rather than looping
forever waiting for it, and each always prints exactly one `RESULT` line
before dropping into its trailing idle/serve loop:

- `RESULT PASS: dualcore-ipc -- ...` — real evidence: HP serviced >=1 HE
  request (or, on HE, all `ROUND_TRIPS` round-trips completed with 0 reply
  mismatches).
- `RESULT SKIP: dualcore-ipc -- ...` — the peer never showed, or stopped
  responding, within a bounded window: no `mproc_boot` backend was selected
  (`ALP_ERR_NOSUPPORT`) and HE was never released, either core's MHU-1 sender
  link never came ready (`ACCESS_READY`), HP never saw a request, or HP never
  replied to HE's first (or a later) round — states what was locally proven,
  not a failure of this app's code.
- `RESULT FAIL: dualcore-ipc -- ...` — a real local error: `alp_mproc_boot_core`
  (HP) returned an unexpected rc (a real SE refusal comes back as
  `ALP_ERR_IO` and lands here, not in the skip above), or a reply's payload
  did not match `request.payload + 1` (a correctness bug, not an absent peer).

If `alp_mproc_boot_core` returns `ALP_ERR_NOSUPPORT` (`rc=-6`), HP reports
that as `RESULT SKIP`, not `RESULT FAIL` — but `-6` here means no `mproc_boot`
backend was selected for HE (an environment/config state:
`alp_mproc_boot_core()` in `src/mproc_dispatch.c` returns `ALP_ERR_NOSUPPORT`
only when backend resolution finds no `mproc_boot` ops for the SoC), **not** a
boot-authority refusal: `se_rc_to_alp()`
(`src/backends/mproc/alif_se_boot.c`) never maps a real SE error to `-6` for
`ALP_CORE_M55_HE`, and the SE boot path itself is bench-proven working on E8
silicon (a peer M55 started and ran an RPMsg link for 495 consecutive
PING/PONG round-trips). Why a given bench observed `-6` is a separate,
unproven question this README doesn't claim to answer.
Every wait for the peer (HP's/HE's MHU-1 sender-link-ready wait: 3000 ms; HP's
request verdict window: 3000 ms; HE's per-round reply wait: 200 ms) is
bounded, so a genuinely absent peer or a sender link that never comes ready
produces a verdict instead of a hang.

`REQ_MBOX`/`RPL_MBOX` are fixed absolute global-SRAM0 addresses, not `.bss` —
Zephyr startup does NOT zero them. Both cores explicitly zero BOTH mailboxes
at the top of `main()`, before any doorbell exchange: on a warm reset or a
bench re-run without a full power cycle, leftover bytes from a PRIOR
successful run would otherwise satisfy "seq changed and is nonzero" on the
very first poll and produce a false PASS with the peer never having booted
this time.
