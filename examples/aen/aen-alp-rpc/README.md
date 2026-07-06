<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-alp-rpc — framed RPC between the two E8 M55 cores via `<alp/rpc.h>`

The same RTSS-HP ↔ RTSS-HE ping/pong as
[`aen-rpc-pingpong`](../aen-rpc-pingpong/) (PR#205, 16/16 PASS) — but driven
through the **portable `<alp/rpc.h>` surface** instead of the raw Zephyr
`ipc_service` calls. This is the teaching example for the framed-RPC API: it
shows customer code calling a handful of vendor-clean `alp_rpc_*` functions
while the alp_rpc Zephyr backend (`src/backends/rpc/zephyr_drv.c`) does the
exact same `ipc_service_open_instance()` + `ipc_service_register_endpoint()`
dance the raw example does — over the very same `alif,mhuv2-mbox` MBOX driver +
shared SRAM0 vring carve-out wired in the board overlays.

## What this example demonstrates

| Raw `aen-rpc-pingpong`                       | Portable `aen-alp-rpc`                       |
| -------------------------------------------- | -------------------------------------------- |
| `DEVICE_DT_GET(DT_NODELABEL(ipc0))`          | `alp_rpc_open(&(alp_rpc_config_t){ .name }) `|
| `ipc_service_open_instance()`                | (inside `alp_rpc_open`)                       |
| `ipc_service_register_endpoint()`            | (inside `alp_rpc_open`)                       |
| `ipc_service_send(&ept, ...)`                | `alp_rpc_send(ch, "ping", ...)`              |
| `on_recv()` switch on direction             | `alp_rpc_subscribe(ch, "pong", on_pong, ...)`|

The raw example put both the wire routing and the direction in one ipc endpoint
named `pingpong`. The framed surface keeps the ipc endpoint name as the channel
**identity** (both cores open the same `alp_rpc_config_t::name`, so their
endpoints bind) and moves per-message routing into the in-frame ASCII method
header. One channel `alp_pingpong` carries two methods: HP → HE `ping`, HE → HP
`pong`. `alp_rpc_subscribe()` filters by method, so each side's callback fires
only for the direction it consumes.

- **HP** (host): boots HE (the portable `alp_mproc_boot_core`,
  SE-boot-service-backed on AEN), `alp_rpc_open`, subscribes
  `pong`, sends 16 `ping`s, counts `pong`s.
- **HE** (remote): `alp_rpc_open`, subscribes `ping`, echoes each as a `pong`
  via `alp_rpc_send`.

## Transport (identical to the proven pingpong)

The board overlays are the bench-proven pingpong config byte-for-byte: the
non-secure MHU-1 per-core alias (both cores TX `0x400B0000` / RX `0x400A0000`,
fabric cross-routed), a 64 KiB `sram_ipc0` vring carve-out in global SRAM0
`@0x02010000`, and `chosen { zephyr,ipc = &ipc0 }` — the single hook
`alp_rpc_open()` resolves. `CONFIG_DCACHE=n` +
`CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET=y` make the shared vrings coherent
+ zeroed. The three MBOX-driver fixes the OpenAMP backend needed
(combined-IRQ enable, RX poll-timer, ACCESS_REQUEST-before-ring) are documented
in the pingpong README and live in `mbox_alif_mhuv2.c`.

## Expected result (bench RAM-run)

```
[HP] boot_cpu rc=0
[HP] alp_rpc_open OK
[HP] subscribe rc=0
RESULT: alp-rpc PASS -- pongs=16/16
```

The app UART is not on USB on this bench, so the console is the RAM console;
liveness/result also mirror to global-SRAM0 beacons (read `ram_console_buf` and
the beacons over SWD). HE's console is in HE-local memory, hence the beacons.

## Build

`open-amp` + `libmetal` are alp-sdk west projects (the manifest allowlists the
Zephyr-pinned modules), so a `west update` fetches them and they are
auto-discovered — no manual `-DEXTRA_ZEPHYR_MODULES` for them:

```sh
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-alp-rpc -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-alp-rpc -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

Package as a dual ATOC (HP `["load","boot"]` @0x50000000 + HE `["load"]`
@0x58000000), `app-gen-toc` + `app-write-mram`; restore the canonical slot0
after. (Bench steps are off-limits in CI; see the pingpong README for the full
flash recipe.)
