<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-rpc-pingpong — OpenAMP RPMsg between the two E8 M55 cores

A working Zephyr `ipc_service` / OpenAMP-RPMsg ping/pong between RTSS-HP and
RTSS-HE on the Alif Ensemble E8 (AEN801), over the `alif,mhuv2-mbox` MBOX driver
+ a shared SRAM0 vring carve-out. Resolves **#45** (mailbox.controller TBD) and
**#50** (`alp_rpc_open` → NOT_READY): the `ipc0` node's `mboxes` point at the
real MHU windows, the RPMsg static-vrings backend binds, and endpoints exchange
data.

- **HP** (host): boots HE (the portable `alp_mproc_boot_core`,
  SE-boot-service-backed on AEN), opens `ipc0`, registers the
  `pingpong` endpoint, sends 16 `ping`s, counts `pong`s.
- **HE** (remote): opens `ipc0`, registers `pingpong`, echoes each `ping` as a `pong`.

## Result (bench-verified on E8, 2026-06-18)

```
[HP] boot_cpu rc=0
[HP] register_endpoint rc=0
[HP] bound=1
RESULT PASS: pingpong -- received 16/16 pong(s) after sending 16 ping(s)
```

Both endpoints bind; all 16 ping/pong round-trips complete. (Beacons mirror
`bound`/count to global SRAM0 since HE's console is in HE-local memory.)

## Verdicts, timeouts, and the HE<->HP boot block on this bench

Both HP and HE always print exactly one `RESULT` line before `main()` returns
(no more idling forever with no verdict):

- `RESULT PASS: pingpong -- ...` — real evidence: HP received all 16 pongs
  (or, on HE, every ping received was queued back to HP -- `ipc_service_send()`
  returning >= 0 only means the frame reached the local vring, HE never
  observes whether HP actually accepted it).
- `RESULT SKIP: pingpong -- ...` — HE was released but the endpoint never
  bound, or it bound but sent nothing, within a bounded window — states what
  was locally proven, not a failure of this app's code.
- `RESULT FAIL: pingpong -- ...` — a real local error: `alp_mproc_boot_core`
  (HP) returned an unexpected rc, `ipc0`/`register_endpoint` failed, or every
  local send (`ipc_service_send`) failed.

This build ships `CONFIG_HAS_ALIF_SE_SERVICES=y` and no `native_sim` overlay,
so `alp_mproc_boot_core()` always resolves to the E8 SE backend for
`ALP_CORE_M55_HE` — the `<alp/mproc.h>` contract's `ALP_ERR_NOSUPPORT` case
("no boot authority for `core` in this build: wrong SoM, `native_sim`, or a
core the platform boots by other means") is not reachable in this
configuration. So HP treats *any* nonzero `alp_mproc_boot_core` rc, including
`ALP_ERR_NOSUPPORT`, as `RESULT FAIL`, not a skip: on these boards a `-6`
here would mean the boot path fell out of the build (e.g. the SE backend lost
the link, or the silicon-ref stopped matching) — a regression this app must
surface, not paper over.

Every wait (register_endpoint retry: 4000 ms; endpoint bind: 5000 ms; HP's
round-drive grace window: 5 extra heartbeats; HE's serve window: 3000 ms) is
bounded so a genuinely absent peer produces a verdict instead of a hang. The
verdict is also mirrored into a beacon word (`SELF_BEACON[2]`, right after the
heartbeat word) before `main()` returns, so a bench SWD read can tell a
completed run (word set to 1/2/3) apart from a crash (word still 0) even
though the heartbeat itself stops moving once `main()` has exited.

## Three MBOX-driver fixes this bring-up required (all in `mbox_alif_mhuv2.c`)

The OpenAMP backend is purely doorbell-driven, which exposed three real bugs in
the Alif MHUv2 MBOX driver — found by bench-tracing the static-vrings handshake:

1. **Combined-interrupt enable.** Unmasking a channel (`CH0_MASK_CLEAR`) is not
   enough; the receiver also needs `INT_EN.CHCOMB` (+0xF98 bit2) set — per the
   Alif DFP `drivers/source/mhu_receiver.c`.
2. **Poll the receiver.** Even fully configured, the non-secure HE↔HP MHU-1
   **RX combined IRQ does not fire on this silicon** (bench-confirmed: the
   doorbell status bit sets, but no NVIC interrupt). The driver therefore drives
   its dispatch from a 1 ms **poll timer** on the raw `CH0_STAT` (the only
   bench-proven RX register), independent of the dead IRQ.
3. **Wake the link before ringing.** The sender must assert `ACCESS_REQUEST` and
   spin for `ACCESS_READY` before each `CH0_SET`, or the doorbell does not
   propagate (the ipc backend never calls `set_enabled` on the TX frame).

The transport rides the non-secure MHU-1 per-core alias (both cores TX
`0x400B0000` / RX `0x400A0000`, fabric cross-routed) — the same pair proven in
`aen-dualcore-ipc`. `CONFIG_DCACHE=n` + `CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET=y`
make the shared `sram_ipc0` vrings coherent + zeroed.

## Build

`open-amp` + `libmetal` are alp-sdk west projects (the manifest allowlists the
Zephyr-pinned modules), so a `west update` fetches them and they are
auto-discovered — no manual `-DEXTRA_ZEPHYR_MODULES` for them:

```sh
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-rpc-pingpong -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-rpc-pingpong -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

Package as a dual ATOC (HP `["load","boot"]` @0x50000000 + HE `["load"]`
@0x58000000), `app-gen-toc` + `app-write-mram`; restore the canonical slot0 after.
