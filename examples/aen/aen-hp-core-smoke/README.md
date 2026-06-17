# aen-hp-core-smoke — M55-HP first light

First-light smoke for the **M55-HP** (RTSS-HP) core of the E1M-AEN801 (Alif
Ensemble E8). Every other AEN bench app runs on the **M55-HE** core; this is the
first alp-sdk image on the **second** M55.

## The HP boot model

The HP core is **held in reset at power-on** — the J-Link AP map shows only the
HE core's AHB-AP with a readable CPUID; the HP core's AP reads no CPUID until it
is released. It is released by the **Secure Enclave** booting an `M55_HP` ATOC:

| | M55-HE (the usual target) | M55-HP (this app) |
|---|---|---|
| `cpu_id` | `M55_HE` | `M55_HP` |
| `loadAddress` | `0x58000000` (HE ITCM global) | `0x50000000` (HP ITCM global) |
| ITCM / DTCM global base | `0x58000000` / `0x58800000` | `0x50000000` / `0x50800000` |

The image is ITCM-linked (`zephyr,flash = &itcm`); the SES loads it to the HP
ITCM global base and starts the HP core (which then sees its ITCM at local `0x0`,
so `VTOR == 0`).

## How first light is observed

The bench reads memory over SWD via the HE/system debug AP, **not** the HP core's
AP, so this app does not depend on the HP RAM console being reachable. It writes
a **liveness beacon** to global SRAM0 (`0x02000000`) — always-on on-chip SRAM at
the same address from every master:

| word | meaning |
|---|---|
| `SRAM0[0]` | magic `0xA11FE000` |
| `SRAM0[1]` | `SCB.CPUID` (`0x411FD220` = Cortex-M55) |
| `SRAM0[2]` | `SCB.VTOR` (`0` — HP runs from its local ITCM) |
| `SRAM0[3]` | heartbeat, `+1` every ~100 ms |

Reading `SRAM0[3]` twice and seeing it **advance** proves the HP core is actively
executing, not a stale value left by a prior image.

## Build + flash

```bash
# Build for the HP board target (note rtss_hp, not rtss_he):
ZEPHYR_BASE=<zephyr> west build \
  -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/aen/aen-hp-core-smoke -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-hp-core-smoke/boards/alp_e1m_aen801_m55_hp.overlay

# Flash as an M55_HP ATOC + read the beacon (SETOOLS license-gated; export SETOOLS_DIR):
scripts/bench/aen/flash-jlink-hp.sh build/aen-hp-core-smoke
```

> **Bench-verified on E8 (2026-06-17):** beacon magic + CPUID present, heartbeat
> advanced across the re-read — the HP core's first light.

An HP ATOC becomes the **active** boot image (the SES boots HP instead of the
slot0 self-test). After HP bring-up, restore the canonical image:
`scripts/bench/aen/flash-jlink-mramxip.sh <person_detect-build-dir>`.

This unblocks the **HE↔HP MHUv2 doorbell** bring-up (which needs both cores
running).
