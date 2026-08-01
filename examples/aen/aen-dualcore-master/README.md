<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-master — boot BOTH M55 cores via the runtime SE boot service (B1)

`aen-dualcore-probe` proved a dual-boot ATOC boots only **one** core. This is the
fix: the SES-booted core starts the **other** M55 at runtime through the portable
`alp_mproc_boot_core(core, entry_addr)` (`<alp/mproc.h>`). On AEN the backend
registry routes that to the Secure Enclave's boot service over the `seservice0`
MHU — `se_service_boot_cpu()` (`SERVICE_BOOT_CPU`), a wrapper added to hal_alif
by the alp-sdk west patch
`zephyr/patches/hal_alif/0001-se-service-add-boot-cpu.patch`. The app itself
carries **no vendor include**.

On the E8 the SES boots the **HP** entry from a dual ATOC, so the **HP build is the
master** — it releases HE. The app is board-aware (the HE build would release HP).
The partner is the `aen-dualcore-probe` build for the other core, packaged
`["load"]`-only (the SES loads it but does not auto-boot it). This is the plain
`se_service_boot_cpu()` (service 501) release path, and for the HP-master →
HE-peer direction shown here it works fine with the DEFAULT config
(bench-proven 2026-06-17 and re-confirmed 2026-08-01).

```sh
# master (HP) + the HE probe as the released core:
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-dualcore-master -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-dualcore-probe  -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# dual ATOC: HP-APP ["load","boot"] @0x50000000 ; HE-APP ["load"] @0x58000000 ; app-gen-toc + app-write-mram
```

**The reverse direction (HE master → HP peer) needs a different config and ATOC.**
The plain 501 path above cannot release an HP peer at all (see the next section);
the only proven way is `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y`
(see `examples/aen/aen-dualcore-master/testcase.yaml`), which defaults the
deferred-TOC path ON and requires the HP peer's ATOC entry to be flagged
`["load","boot","deferred"]` instead of `["load"]`:

```sh
# master (HE) + the HP probe as the released, deferred-TOC peer:
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-dualcore-master -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" -DCONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-dualcore-probe  -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# dual ATOC: HE-APP ["load","boot"] @0x58000000 ; HP-APP ["load","boot","deferred"] @0x50000000 ; app-gen-toc + app-write-mram
```

This HE-master combination is not yet bench-run (silicon-code-verified, not
bench-verified) -- see `docs/aen-bench-bringup.md` § Flow A for the
bench-proven case (`aen-rpc-pingpong`, same deferred-TOC mechanism, HP master
releasing a deferred HE peer).

**Related:** `aen-dualcore-he-master` packages this exact HE-master ->
HP-peer combination as its own self-contained example (one app, both roles,
`PEER_IS_HP=y` on by construction instead of a `-D` override) and is the one
that's actually bench-proven for this direction (2026-08-01) -- start there
if this is the direction you need.

## Result (bench-verified on E8, 2026-06-18) — BOTH cores run ✅

```
HP @0x02000010 : B1B10090 (magic)  heartbeat 0x0812 -> 0x090C (HP master runs)
               + B007C0DE (boot magic)  rc=0x00000000   (boot request accepted)
HE @0x02001010 : B1B100E0 (magic)  heartbeat 0x03E0 -> 0x0458 (HE RELEASED, runs)
```

The HP master (booted by the SES) issued the HE boot request @ `0x58000000` →
`rc=0` → the HE core came up and advanced its heartbeat. **Both M55s are live
from one power-on** — B1 unblocked. This is the route a dual-core RPC / HE↔HP
doorbell builds on (the second core is no longer dark). (Bench run predates the
portable-wrapper conversion; the wrapper issues the identical SE request.)

The core-id mapping (`ALP_CORE_M55_HP` → `EXTSYS_0=2`, `ALP_CORE_M55_HE` →
`EXTSYS_1=3`, hal_alif `services_lib_api.h`) lives in the SDK backend
(`src/backends/mproc/alif_se_boot.c`). `entry_addr` is the target core's
image/VTOR base (its ITCM global alias: HE `0x58000000`, HP `0x50000000`).
There is **no bare M55 register** to release the other core — it is SE-mediated
only.

## One correctness requirement (bench-found)

**`CONFIG_DCACHE=n`.** The peer's heartbeat beacon is read/written by both
cores, each with its own D-cache → the master's cross-core read of the peer's
heartbeat saw a stale value (the boot request was accepted but the released
core's own heartbeat never appeared to advance, i.e. `RESULT SKIP`, even
though it was live and running). Disabling the D-cache makes the shared SRAM0
region coherent (the AEN-SRAM precedent). A cache-on variant would
`sys_cache_data_flush_range` on the writer + `invd_range` on the reader, on
silicon where the D-cache can be enabled at all.

## Verdicts, timeouts, and the HE↔HP boot block on this bench

The master always prints exactly one `RESULT` line before dropping into its
trailing heartbeat loop:

- `RESULT PASS: dualcore-master -- ...` — real evidence: `alp_mproc_boot_core`
  accepted the request AND the peer's own heartbeat word was observed to
  advance within the bound below (the request being accepted, alone, does
  not prove the peer actually came up).
- `RESULT SKIP: dualcore-master -- ...` — the request was accepted but the
  peer's heartbeat never advanced within the bound — states what was locally
  proven, not a failure of this app's code.
- `RESULT FAIL: alp_mproc_boot_core rc=%d` — a real local error: `boot_core`
  returned an unexpected rc (`ALP_ERR_NOSUPPORT` included -- see below).

With the **default** config (`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC`
off), this build ships `CONFIG_HAS_ALIF_SE_SERVICES=y` and no `native_sim`
overlay, so `alp_mproc_boot_core()` always resolves to the E8 SE backend's
plain `se_service_boot_cpu()` path for either M55 core — the `<alp/mproc.h>`
contract's `ALP_ERR_NOSUPPORT` case ("no boot authority for `core` in this
build: wrong SoM, `native_sim`, or a core the platform boots by other means")
is not reachable there. So the master treats *any* nonzero
`alp_mproc_boot_core` rc as `RESULT FAIL`, not a skip: on these boards a
nonzero rc here would mean the boot path fell out of the build (e.g. the SE
backend lost the link, or the silicon-ref stopped matching) — a regression
this app must surface, not paper over.

That said, an HE build releasing an HP peer via this default config is
**accepted** (`rc=ALP_OK`, not a nonzero rc, so the FAIL check above doesn't
fire) but the peer never comes up: Alif's SE Host Services API docs (v1.109.0
p.115) document that `se_service_boot_cpu()` (service 501) invalidates the HP
core's TCM on release, so the peer locks up before it can advance its
heartbeat, and this is reported as `RESULT SKIP` (accepted, peer never
advanced) — not `RESULT FAIL`, since the local request path is fine.

With the deferred-TOC path ON
(`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y`, see above),
`ALP_ERR_NOSUPPORT` DOES become reachable: a build whose `PEER_IS_HP` doesn't
match which core this role's `TARGET_CORE` actually releases is a
build-config bug, not a bench/silicon state, and now correctly falls into
`RESULT FAIL` via the same nonzero-rc check as any other real local error.

The peer-heartbeat wait (2000 ms, polled every 20 ms) is bounded, so an
accepted request whose peer never actually comes up produces a verdict
instead of a hang. To make the HE build actually release an HP peer, use the
deferred-TOC path (`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y`,
see above and `docs/aen-bench-bringup.md`).
