<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-he-master — HE releases HP at runtime (the direction every other AEN dual-core example doesn't do)

All seven other AEN dual-core examples (`aen-dualcore-master`, `aen-dualcore-probe`,
`aen-dualcore-doorbell`, `aen-dualcore-ipc`, ...) release their peer
**HP-master → HE-peer**. This example is the reverse: **HE-master → HP-peer**,
via the portable `alp_mproc_boot_core(core, entry_addr)` (`<alp/mproc.h>`) —
self-contained, so a reader doesn't have to discover it by hand-pairing
`aen-dualcore-master`'s HE build with `aen-dualcore-probe`'s HP build (which is
how this direction was first bench-proven).

## Why this direction needs a different release path

The plain release path every HP-master → HE-peer sibling uses —
`["load","boot"]` + `se_service_boot_cpu()` (service 501) — **cannot** release
an HP peer at all. Alif's SE Host Services API docs
(`SE_Host_Services_API_v1.109.0.pdf`):

- p.112, `SERVICES_boot_cpu`: *"This service does not perform image loading,
  verification, etc., it just boots the core... You would need to use an ATOC
  to achieve these."*
- p.113, `SERVICES_boot_cpu`: *"For the M55 cores, there are cases in which
  this service does not work. The currently known case is the **M55-HP core
  in FUSION REV_Bx devices**, where resetting the core also invalidates its
  TCM content."*
- p.115, `SERVICES_boot_release_cpu`: *"in some cases, resetting the core
  also invalidates its TCM. A known case is the **M55-HP core in Ensemble
  devices**. Because of that, after calling `SERVICES_boot_reset_cpu()` to
  stop the core, the image in the TCM must be reloaded, before calling
  `SERVICES_boot_release_cpu()` to start the core."*

**The two vendor passages disagree with each other on device scope** — p.113
says "FUSION REV_Bx devices", p.115 says "Ensemble devices" with no
qualifier — and that disagreement is quoted here rather than resolved. The
E1M-AEN801 is an Ensemble part, so p.115 covers it either way. Bench-measured
on E8 (2026-07-31, HE master releasing an HP peer via 501): the SES table
reported the HP entry `uLV` (Loaded, Verified), but its ITCM read as
uninitialized SRAM, and releasing it vectored the core from empty memory —
`CFSR=0x00000101` (IACCVIOL+IBUSERR), `PC=0xEFFFFFFE`.

**The fix** is p.112's `SERVICES_boot_process_toc_entry` (service 500,
`se_service_process_toc_entry()`) — the vendor calls it *"a higher-level...
convenient way to boot a CPU core"* — against a peer ATOC entry flagged
`["load","boot","deferred"]`. Per the SETOOLS guide (`AUGD0005` p.35),
`deferred` means the SES **skips the entry entirely at boot** ("no boot OR
LOAD") instead of loading it and releasing it later; the master then un-defers
it at runtime with ONE call that loads, verifies, AND releases together,
strictly **after** whatever reset that release involves — the reset → reload
→ release order p.115's own remedy requires, and the exact opposite of what
the plain path does (load at power-on, reset on release).

## The ATOC shape

```json
"ALP-HE": { "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] },
"ALP-HP": { "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot", "deferred"] }
```

`ALP-HE` is a normal entry — the SES loads and releases it at power-on like
any single-core image; this is the MASTER role. `ALP-HP` carries `deferred`:
nothing is placed in its ITCM at power-on (SES table shows `uLs  D`, Dest
Addr blank, Time `0.00 ms`); it stays inert until the HE master's
`alp_mproc_boot_core(ALP_CORE_M55_HP, 0x50000000)` call un-defers it via
service 500.

**`"ALP-HP"` is not a free-choice label** — it is the ATOC entry *key*
`se_service_process_toc_entry()` looks up at runtime, and it must match
`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_ENTRY_ID` **exactly**
(default `"ALP-HP"` once `..._PEER_IS_HP=y`, see
`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.conf`). The SE service
takes an 8-byte image id (`IMAGE_NAME_LENGTH`, hal_alif
`services_lib_protocol.h`) and truncates silently — a mismatched key doesn't
error, it just un-defers nothing, and the peer sits deferred forever with no
local error to report (`alp_mproc_boot_core` still returns `ALP_OK`; only the
peer's beacon staying flat gives it away — see Result below).

## Build

```sh
# master (HE):
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-dualcore-he-master -d build/he -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# peer (HP), deferred:
west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-dualcore-he-master -d build/hp -- "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# dual ATOC: ALP-HE ["load","boot"] @0x58000000 ; ALP-HP ["load","boot","deferred"] @0x50000000 ; app-gen-toc + app-write-mram
```

Only the HE build needs `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP=y`
— it's the board `.conf` default there (see
`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.conf`), not a `-D`
override; no separate `west build` flag is needed. The HP build carries no
SE-service Kconfig at all — it never calls a boot API, it only stamps a
beacon once un-deferred (see `src/main.c`).

## Result — silicon-proven on E8, 2026-08-01 (this exact self-contained example)

The direction was first proven by hand-pairing `aen-dualcore-master`'s HE
build with `aen-dualcore-probe`'s HP build; this self-contained example was
then bench-run on its own, from a from-scratch rebuild (no prebuilt
artefacts were on the bench host):

```
|   ALP-HE | M55-HE | 0x805665C0 | 0x80565BC0 | 0x58000000 | 0x58000000 | 83952 | uLVB   | 26.86 |
|   ALP-HP | M55-HP | ---------- | 0x8057ADB0 | ---------- | ---------- | 18360 | uLs  D |  0.00 |

02000010 = B1B10090 000027A6  ->  B1B10090 00002CDF    (+8 s, repeat-read)
alp_mproc_boot_core(M55-HP, 0x50000000) rc=0
RESULT PASS: dualcore-he-master -- M55-HP booted and heartbeating (peer beacon magic=0xb1b10090)
```

The SES table row for `ALP-HP` shows `uLs  D` (Loaded-Skipped, Deferred),
Dest Addr blank, Time `0.00 ms` — confirming nothing was placed at power-on.
Reading `0x02000010` twice eight seconds apart shows the heartbeat word
(`0x000027A6` → `0x00002CDF`) advancing while the magic (`0xB1B10090`) stays
constant — the peer is up and running, not merely loaded.

Two more direct-evidence details from this run, beyond the SES table:

- **At SES-boot time, global HP ITCM `0x50000000` held un-loaded junk**
  (`FE8C1E42 7184989D ...`) while HE's `0x58000000` byte-matched its `.bin`
  exactly. Not an inference from the SES table's `D` flag — a direct read of
  the ITCM contents confirming the SES genuinely skips a deferred entry at
  power-on, and that `se_service_process_toc_entry()` genuinely places it at
  runtime, not merely marks it released.
- **The bench rebuild was bit-exact.** The bench host had no prebuilt
  artefacts and rebuilt both roles from this example's source; the resulting
  `zephyr.bin` md5s (`3023a9d8d2e7d730c41804840f22a291` HE,
  `359f9618751d4e00bb2682dd8c34f670` HP) matched the authoring build exactly
  — the source alone reproduces the binaries, nothing bench-local leaked in.

**A J-Link reset-pin reset is NOT a valid first-light for this app.** On that
path the SES table read correctly, but the HE image never actually executed:
`ram_console_buf` stayed uninitialized, the beacon read all zeros, and
`CFSR = 0x00000000` (no fault — the core just never ran the image). Only a
cold power-cycle produced the clean `RESULT PASS` above. Anyone reproducing
this on the bench needs a cold power-cycle, not a reset-pin toggle.

This app reports the same verdict itself, in-band, instead of requiring a
human SWD read:

- `RESULT PASS: dualcore-he-master -- M55-HP booted and heartbeating (peer
  beacon magic=0xb1b10090)` — `alp_mproc_boot_core` returned `ALP_OK` AND the
  HP peer's own beacon word (`0x02000010`) was observed to advance within
  2000 ms.
- `RESULT SKIP: dualcore-he-master -- alp_mproc_boot_core(M55-HP) accepted
  (rc=0) but its beacon (0x02000010) did not advance within 2000 ms` — states
  what was observed (the beacon word did not move within the bound), never an
  inferred cause. Do **not** read this as "the peer never ran": that
  inference was measured wrong on this exact family of examples — see the
  `CONFIG_DCACHE=n` note below.
- `RESULT FAIL: alp_mproc_boot_core rc=%d` — `boot_core()` itself refused the
  request: either a build-config bug (this build's `PEER_IS_HP` not matching
  the flashed ATOC's deferred entry key, see the ATOC-shape section above) or
  a real backend regression.

## `CONFIG_DCACHE=n` — non-negotiable, both boards

Two independent reasons, both in `prj.conf`:

1. The E8 D-cache maintenance loop hangs on this silicon (a shared bench
   rule across the whole AEN example tree — see `aen-hp-core-smoke`,
   `aen-gpu2d-bench`).
2. The beacons this app polls (its own + the peer's) live in cross-core
   global SRAM0, which is **not** coherent between the M55-HE and M55-HP
   D-caches. With the cache on, a stale line can make an actually-advancing
   peer beacon read as flat forever — the exact false negative PR #1080
   found and fixed on three sibling dual-core examples (`aen-dualcore-master`,
   `aen-dualcore-probe`, `aen-dualcore-doorbell`; their `SKIP` text used to
   claim "peer never ran" / "peer image absent/not running" when the peer
   was, in fact, running the whole time). This example ships
   `CONFIG_DCACHE=n` from the start specifically so it never teaches that
   mistake.

## Related

- `aen-dualcore-master` — the symmetric app (HP-master → HE-peer works with
  the *default* config; HE-master → HP-peer needs the same deferred-TOC
  Kconfig this example defaults on its own).
- `aen-rpc-pingpong` — the other in-tree deferred-TOC consumer (HP-master →
  HE-peer direction, RPMsg PING/PONG over the released link).
- `docs/aen-bench-bringup.md` § Flow A — Dual-core deferred-TOC boot — the
  full asymmetry table and vendor citations.
- `src/backends/mproc/alif_se_boot.c` — the backend implementing both release
  paths behind the portable `alp_mproc_boot_core()`.
