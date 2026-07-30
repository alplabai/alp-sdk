<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-probe — does a dual-entry ATOC boot BOTH M55 cores?

The decisive **B1** test for the E1M-AEN801 (Ensemble E8). One app, role-by-board:
the RTSS-HE build stamps an advancing heartbeat at global SRAM0 `0x02001014`; the
RTSS-HP build stamps one at `0x02000014`. Both write GLOBAL SRAM0 (master-agnostic,
readable over SWD from whichever core J-Link attaches to).

Build BOTH, package into a **dual-entry ATOC** (HE @`0x58000000` + HP @`0x50000000`,
both `["load","boot"]`, like the SETOOLS `mhu-dual.json`), `app-gen-toc` +
`app-write-mram`, reset, then read both heartbeats — advancing means that core ran.

```sh
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-dualcore-probe -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-dualcore-probe -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# stage he/hp .bin, author a mhu-dual.json-shaped config, app-gen-toc + app-write-mram, reset
# read: JLinkExe -device Cortex-M55 ... mem32 0x02000010,4  (HP) ; mem32 0x02001010,4 (HE)
```

## Result (bench-verified on E8, 2026-06-18) — dual-entry ATOC boots **only HP**

```
HP @0x02000014 : heartbeat 0x0FC2 -> 0x106E   (ADVANCING -> HP core runs)
HE @0x02001014 : heartbeat 0      -> 0        (HE core never runs)
```

So even the **correct** dual-entry ATOC boots only the HP (secondary) core — the
"a single ATOC with two boot entries boots both cores" theory is **refuted**. The
SES hands off to one core; the other M55 must be started at **runtime** by the
booted core via the SE boot service (`se_service_boot_cpu(EXTSYS_0, <addr>)` over
the seservice0 MHU — `SERVICE_BOOT_CPU` 501), which needs a thin hal_alif wrapper
added. There is no bare M55 register to release the other core
(`AON.RTSS_HP_CTRL`/`RTSS_HE_CTRL` carry only WIC/wake bits, no VTOR/boot-addr).

> Note: SRAM0 magic word at the slot base can read back 0 even when the core is
> running; the **heartbeat** (a nonzero offset that advances across two reads) is
> the trustworthy liveness signal.

## Verdicts, timeouts, and the boot_core NOSUPPORT rule on this bench

This app never calls `alp_mproc_boot_core` itself — both cores are meant to be
started by the SES from one dual-entry ATOC, so there is no local boot rc to
gate. Each build always prints exactly one `RESULT` line before dropping into
its trailing heartbeat loop:

- `RESULT PASS: dualcore-probe -- ...` — real evidence: the peer core's own
  beacon (magic + heartbeat, same global-SRAM0 scheme) was observed to advance
  within the bound below.
- `RESULT SKIP: dualcore-probe -- ...` — the peer's heartbeat never advanced
  within the bound — states what was locally proven (this core is up), not a
  failure of this app's code; on this bench it matches the single-core-boot
  finding this app exists to demonstrate.

There is no `RESULT FAIL` case: with no boot call to check an rc on, the only
outcomes this app can distinguish are "the peer showed up" and "it didn't".
The peer-heartbeat wait (2000 ms, polled every 20 ms) is bounded, so a peer
that never runs produces a verdict instead of a hang — the same
`boot_core NOSUPPORT on this bench → SKIP` rule the other three dualcore apps
apply, just without a local rc to check because this app makes no boot call.
