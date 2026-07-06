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
RESULT: pingpong PASS -- pongs=16/16
```

Both endpoints bind; all 16 ping/pong round-trips complete. (Beacons mirror
`bound`/count to global SRAM0 since HE's console is in HE-local memory.)

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
