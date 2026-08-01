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

## Result (bench-verified on E8, 2026-08-01) — dual-entry ATOC boots BOTH cores ✅

```
HP : RESULT PASS: dualcore-probe -- both M55 cores advancing (this=HP, peer magic=0xb1b100e0 moved) -- dual-entry ATOC booted both
HE : RESULT PASS: dualcore-probe -- both M55 cores advancing (this=HE, peer magic=0xb1b10090 moved) -- dual-entry ATOC booted both
```

Both cores stamp their own beacon and observe the other's advancing within
the 2000 ms window; SES boot-header capture over the SE-UART additionally
showed `uLVB` on **both** ATOC entries. **This supersedes the 2026-06-18
finding below.** The dual-entry ATOC was booting both cores all along — the
missing piece was `CONFIG_DCACHE=n` (see "One correctness requirement"
below): without it, each core's cross-core read of the other's beacon saw a
stale, non-advancing value even though both cores were genuinely live,
producing the "only HP advances" reading recorded historically.

> Note: SRAM0 magic word at the slot base can read back 0 even when the core is
> running; the **heartbeat** (a nonzero offset that advances across two reads) is
> the trustworthy liveness signal.

### Historical: 2026-06-18 reading (SUPERSEDED 2026-08-01 — see above)

```
HP @0x02000014 : heartbeat 0x0FC2 -> 0x106E   (ADVANCING -> HP core runs)
HE @0x02001014 : heartbeat 0      -> 0        (HE core never runs)
```

At the time this read as: even the correct dual-entry ATOC boots only the HP
(secondary) core — the "a single ATOC with two boot entries boots both cores"
theory refuted, the SES hands off to one core, the other M55 must be started
at runtime by the booted core via the SE boot service
(`se_service_boot_cpu(EXTSYS_0, <addr>)` over the seservice0 MHU —
`SERVICE_BOOT_CPU` 501). That mechanism is real and still how
`aen-dualcore-master` brings the peer core up — but the "only HP boots from
a dual-entry ATOC" conclusion itself was an artifact of the D-cache hazard
above, not a property of the ATOC/SES boot path.

## One correctness requirement (bench-found)

**`CONFIG_DCACHE=n`.** Each core's heartbeat beacon is read cross-core by the
other, each with its own D-cache → a cross-core read of the peer's beacon saw
a stale (non-advancing) value even while the peer was genuinely live and
advancing it. Disabling the D-cache makes the shared SRAM0 region coherent
(the AEN-SRAM precedent). A cache-on variant would `sys_cache_data_flush_range`
on the writer + `invd_range` on the reader, on silicon where the D-cache can
be enabled at all.

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
  failure of this app's code. As of `CONFIG_DCACHE=n` (see above) this path
  is unreachable in a correct build on this bench: the peer genuinely was
  live every time it was seen SKIP historically, see "Historical" above.

There is no `RESULT FAIL` case: with no boot call to check an rc on, the only
outcomes this app can distinguish are "the peer showed up" and "it didn't".
The peer-heartbeat wait (2000 ms, polled every 20 ms) is bounded, so a peer
that never runs produces a verdict instead of a hang — the same
`boot_core NOSUPPORT on this bench → SKIP` rule the other three dualcore apps
apply, just without a local rc to check because this app makes no boot call.
