<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# Secure-Enclave (SE) runtime services — E1M-AEN (Alif Ensemble)

How an M55 application talks to the **Secure Enclave (SES)** at *runtime* on an
**E1M-AEN** SoM — querying device identity / lifecycle, reading the power
profile, and (gated) changing DVFS or the secure-boot table. This is the
**`se_service_*` API** path, distinct from the one-time
[`aen-provisioning.md`](aen-provisioning.md) SES→MRAM flash flow.

> Bench-verified on a real **E1M-AEN801** (E8, RTSS-HE) — see the
> `aen-se-service-info` (#192), `aen-se-service-query` (#197) examples and
> [`bring-up-aen.md`](bring-up-aen.md).

## 0. The model

The SES is always running on the SE core. The M55 reaches it over **two Arm
MHUv2 mailboxes** (`seservice0r` RX @0x40040000 IRQ 37, `seservice0s` TX
@0x40050000 IRQ 38), tied together by the `se_service` DT node. The Apache-2.0
hal_alif client (`se_services/zephyr/src/se_service.c`) drives them through
Zephyr's IPM API.

On the alp-sdk (upstream-Zephyr + hal_alif) stack you must enable the in-tree
glue — upstream Zephyr ships none of it:

```ini
CONFIG_ALP_SDK=y
CONFIG_ARM_MHUV2=y            # the in-tree arm,mhuv2 IPM driver
CONFIG_HAS_ALIF_SE_SERVICES=y # compiles hal_alif se_service.c
```

…plus the board overlay that okays `seservice0r` / `seservice0s` / `se_service`
and sets the `itcm`/`dtcm` `global_base` props `local_to_global()` needs. Copy
the overlay from `examples/aen/aen-se-service-info/boards/`. The SE answers even
on a J-Link RAM-run, so these examples validate without a flashed image.

Every `se_service_*` call bounds its wait internally (returns `0` / `-EAGAIN`
(timeout, retry) / `-EBUSY` (SE busy) / a positive SE error code), so a call
never hangs — if the SE is unreachable you get a bounded error, not a lockup.

## 1. Read-only services — safe, zero-risk

These never change device state. All confirmed `rc=0` on the E8 bench (#197):

| Service | Returns | E8 bench value |
| --- | --- | --- |
| `se_service_heartbeat()` | liveness | rc=0 |
| `se_service_get_se_revision(u8 buf[80])` | SES firmware string | `SES A0 v1.106.2 Jul 14 2025` |
| `se_service_get_toc_number(u32*)` | TOC entry count | 5 |
| `se_service_get_toc_version(u32*)` | TOC version | `0x016a0200` |
| `se_service_get_device_part_number(u32*)` | part id | `0x000002a0` |
| `se_service_system_get_device_data(get_device_revision_data_t*)` | LCS + ids + keys digests + serial | LCS `0x01` (**DM**), `ALIF_PN="AE822FA0E5597LS0"` |
| `se_service_get_run_cfg(run_profile_t*)` | live power/clock profile | DCDC **825 mV**, `power_domains=0x16d`, `cpu_clk_freq=4`, `run_clk_src=2` |
| `se_service_get_off_cfg(off_profile_t*)` | standby/wake profile | DCDC 825 mV, no wake/EWIC configured |
| `se_service_get_rnd_num(u8*, len)` | SE TRNG bytes | 8 random bytes |

`se_service_get_last_set_run_cfg()` returns the cached run profile without an SE
round-trip (faster).

**Lifecycle-state (LCS) legend:** `0x0` CM (chip mfr) · `0x1` **DM** (device
mfr, the maker-provisioned state) · `0x5` SE (secure-enabled) · `0x7` RMA.

## 2. Mutating services — GATED (recovery required before any bench run)

These change live device state. **Do not run them on a single bench board
without a sacrificial unit and a proven SETOOLS recovery path** (§3). They are
documented here so the path is review-ready, not so it is run casually.

### 2.1 `se_service_set_run_cfg(run_profile_t*)` — DVFS / power

Sets the run power/clock profile (DCDC voltage 750–850 mV, clock sources, CPU
frequency, power domains, memory retention, IO-flex 3V3).

```c
run_profile_t p;
se_service_get_run_cfg(&p);     /* read the live baseline first */
p.cpu_clk_freq = <target>;      /* change ONE field for DVFS    */
int rc = se_service_set_run_cfg(&p);
```

**Two traps:**

1. **Cache short-circuit.** `set_run_cfg` *skips the SE call entirely if the
   profile equals the cached value* — so re-asserting the value `get_run_cfg`
   just returned is a **no-op that returns `rc=0` without any SE round-trip**.
   It validates nothing about the SE set path; it only proves the wrapper's
   cache logic. To actually exercise the SE you must change a field — which
   changes the live operating point.
2. **Brownout risk.** A `dcdc_voltage` / `cpu_clk_freq` / `run_clk_src` the rail
   can't sustain browns out or hangs the core. Recovery is a **power cycle**
   (the operator must do it physically — a J-Link `loadbin` reset does not
   restore a collapsed rail). Always read → change one field → `set` → re-read
   to confirm → keep the baseline to restore.

`se_service_set_off_cfg()` is the standby twin; same caution.
`se_service_clock_set_divider()` changes a PLL/bus divider directly — same
brownout class.

### 2.2 `se_service_update_stoc(u8 *img, u32 size)` — A/B secure-boot update

Rewrites the **System TOC (STOC)** in MRAM — the customer secure-boot / A-B
field-update path. This is the most destructive SE service: a malformed STOC
leaves the SES with nothing valid to boot → **the module does not boot → brick**,
recoverable only over the SE-UART with SETOOLS (§3), and not always recoverable.

It ties into the SoM-maker provisioning model (the module ships DM-provisioned
with a dev-signed ATOC; see `aen-provisioning.md`) and is the SES-native answer
to customer secure-boot + A/B that the
[`aen-bench-bringup.md`](aen-bench-bringup.md) MCUboot analysis lands on — the
SES verifies the signed slot content cert, so the field-update is a *STOC swap*,
not a software-MCUboot chainload.

```c
/* img/size = a SETOOLS-built, signed STOC image staged in RAM/MRAM. */
int rc = se_service_update_stoc(stoc_img, stoc_size);
/* rc=0 => SES accepted + wrote the new STOC; next boot uses it. */
```

**Do not bench this without:** (a) a sacrificial board, (b) the SETOOLS recovery
chain (§3) proven on that board first, (c) a known-good STOC image to roll back
to. Until then it stays design-only.

### 2.3 Also mutating (out of scope here)

`se_service_boot_es0` / `shutdown_es0` (power a subsystem; needs an NVDS config
blob), `se_service_se_sleep_req` (clears the SE-ready flag — next call
re-syncs), `se_service_boot_reset_soc` / `boot_reset_cpu` (reset),
`se_service_system_set_services_debug` (changes the SE debug posture). All change
state; none are needed for the read-only characterisation.

## 3. Recovery (a bad STOC / collapsed rail)

- **Collapsed rail (bad `set_run_cfg`/divider):** physically power-cycle the
  board, then re-flash via J-Link RAM-run or the SES path. The bad profile is
  not persisted unless you also wrote it to an off/boot config, so a power cycle
  returns the default.
- **Bad STOC (`update_stoc`):** the M55 will not come up. Recover over the
  **SE-UART** with **SETOOLS** — re-provision a known-good STOC
  (`maintenance` / `app-write-mram` per `aen-provisioning.md`). Validate this
  chain on a sacrificial board *before* ever calling `update_stoc` on a unit you
  care about.

## 4. Bench-execution policy (summary)

| Service class | Bench-runnable now? |
| --- | --- |
| §1 read-only queries | **Yes** — zero risk, validated on E8 (#197) |
| §2.1 `set_run_cfg` (real change) | No — needs power-cycle recovery on hand; idempotent re-assert is a cache no-op |
| §2.2 `update_stoc` | No — sacrificial board + proven SETOOLS recovery required first |

See `examples/aen/aen-se-service-info` (transport + LCS) and
`aen-se-service-query` (full read-only surface) for the runnable parts.
