<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-master — boot BOTH M55 cores via the runtime SE boot service (B1)

`aen-dualcore-probe` proved a dual-boot ATOC boots only **one** core. This is the
fix: the SES-booted core starts the **other** M55 at runtime by asking the Secure
Enclave over the `seservice0` MHU — `se_service_boot_cpu(cpu_id, address)`
(`SERVICE_BOOT_CPU`). That wrapper is added to hal_alif by the alp-sdk west patch
`zephyr/patches/hal_alif/0001-se-service-add-boot-cpu.patch`.

On the E8 the SES boots the **HP** entry from a dual ATOC, so the **HP build is the
master** — it releases HE. The app is board-aware (the HE build would release HP).
The partner is the `aen-dualcore-probe` build for the other core, packaged
`["load"]`-only (the SES loads it but does not auto-boot it).

```sh
# master (HP) + the HE probe as the released core:
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-dualcore-master -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-dualcore-probe  -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# dual ATOC: HP-APP ["load","boot"] @0x50000000 ; HE-APP ["load"] @0x58000000 ; app-gen-toc + app-write-mram
```

## Result (bench-verified on E8, 2026-06-18) — BOTH cores run ✅

```
HP @0x02000010 : B1B10090 (magic)  heartbeat 0x0812 -> 0x090C (HP master runs)
               + B007C0DE (boot magic)  rc=0x00000000   (se_service_boot_cpu rc=0)
HE @0x02001010 : B1B100E0 (magic)  heartbeat 0x03E0 -> 0x0458 (HE RELEASED, runs)
```

The HP master (booted by the SES) called `se_service_boot_cpu(EXTSYS_1 /* HE */,
0x58000000)` → `rc=0` → the HE core came up and advanced its heartbeat. **Both
M55s are live from one power-on** — B1 unblocked. This is the route a dual-core
RPC / HE↔HP doorbell builds on (the second core is no longer dark).

`EXTSYS_0=2` is M55-HP, `EXTSYS_1=3` is M55-HE (hal_alif `services_lib_api.h`).
`address` is the target core's image/VTOR base (its ITCM global alias: HE
`0x58000000`, HP `0x50000000`). There is **no bare M55 register** to release the
other core — it is SE-mediated only.
