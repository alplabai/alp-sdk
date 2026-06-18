<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-ipc — bidirectional HE↔HP shared-memory request/response

Real data IPC between the two M55 cores, built on the proven dual-core boot
(`se_service_boot_cpu`) + MHU-1 doorbell. HE (requester) writes a `{seq, len,
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
