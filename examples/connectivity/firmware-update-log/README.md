# firmware-update-log

Portable, tamper-evident firmware-update audit log using `<alp/update_log.h>`.

## One API across SoMs

The same application code works on every Alp SoM. The backend is selected
automatically at boot. On native_sim and on boards without secure storage, the
software tier builds and verifies a tamper-evident chain, but it does not make
old entries physically immutable to application firmware.

The hardware-enforced tier is available when the selected backend has a real
secure owner. The TF-M route keeps entries as write-once secure-world assets
and keeps the high-watermark counter in protected storage. On Alif E4/E8, the
AEN route uses the second M55 as the trusted owner: the application core sends
requests over MHU, the owner writes the MRAM log, and the platform blocks the
application core from writing that MRAM partition directly. That block is
programmed through the OEM ATOC device config, on HE's **master-side** firewall
(FC8) — not the MRAM slave-side firewall, which the SERAM firmware owns. It is
silicon-proven on E8: HE bus-faults on a direct write to the log window while
running normally otherwise. The AEN route reports `HW_ENFORCED` once that FC8
policy is provisioned and the HP owner answers; otherwise it stays
`SW_TAMPER_EVIDENT`. See "Hardware enforcement status" below.

## Assurance levels

| Level | Value | What you get |
|---|---|---|
| `SW_TAMPER_EVIDENT` | `0` | SHA-256 hash-chain + monotonic counter (NVS-persisted where the board provides an `alp_ulog_partition`; RAM otherwise). Detects mutation, truncation, rollback, and reorder of historical entries. App-cooperative: a process with write access to the store can forge entries. Use for audit trails where cooperative tamper-evidence is sufficient. |
| `HW_ENFORCED` | `1` | Trusted-owner store isolated from the application core. Normal application firmware cannot rewrite old entries when the owner and its storage are isolated by the platform. On TF-M this is a secure partition and protected storage. On AEN this is an M55 owner **plus** an MRAM log partition the application core is physically blocked from writing — provisioned on HE's master-side firewall (FC8) via the OEM device config and silicon-proven on E8 (see below). A non-decrementable rollback anchor further depends on a provisioned secure counter or equivalent device policy. |

Call `alp_update_log_assurance()` to query which level is active at runtime.

## Persistence

With `CONFIG_ALP_SDK_UPDATE_LOG_PERSIST` (default y when `CONFIG_NVS` is on)
and a fixed partition labelled `alp_ulog_partition` in the devicetree, the
software store lives in Zephyr NVS: the log and its monotonic counter survive
reboot and firmware update. Boards opt in by carving that partition out of a
writable flash device (offsets are board-specific — see the Kconfig help).
Without the partition the tier falls back to the pre-persistence **in-RAM**
store and the log does not survive a reboot.

Persistence does **not** change the assurance level: the software tier stays
tamper-*evident* — `verify()` catches out-of-band mutation, truncation,
rollback, and reorder, but code that can write the partition can rebuild the
store and counter consistently. App-immutability requires the `HW_ENFORCED`
tier (secure isolation + protected counter).

On the E1M-AEN801 / Alif E8 board, this example carries AEN-specific overlays
for a dual-M55 image. The SE ATOC package loads HP to its ITCM global alias
(`0x50000000`) and loads HE to its ITCM global alias (`0x58000000`). The update
log itself is stored in a dedicated MRAM `alp_ulog_partition` for NVS, kept away
from the top-of-MRAM ATOC package used by the SE boot flow.

The AEN bench config also sets
`CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED=y`. That is deliberate: MRAM/NVS
is durable, but not automatically application-immutable. The HE build enables
the AEN dual-M55 client backend and fails closed unless both conditions are
true:

1. the HP owner image is running and answers the mailbox request; and
2. the board profile enables `CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN`
   after provisioning has locked the MRAM log partition against HE writes and
   the direct-write rejection has been proven on silicon.

That Kconfig bit does not program the firewall. It is the public build-time
latch saying the SE/device firewall policy has already been provisioned and
silicon-proven for this board.

### Hardware enforcement status (E8) — proven

Condition 2 is **satisfiable with OEM tooling** and silicon-proven on E8 (Alp Lab
bench, 2026-07-06). The key was targeting the right firewall component:

- The MRAM **slave-side** firewall (FC13, `0x80000000+`) is owned and opened to the
  application masters by the SERAM firmware, and is excluded from the OEM device
  config (Alif AUGD0005, "Open Firewall configuration"). An OEM FC13 region over
  the log window is accepted into the table but does not gate the write — the early
  probes confirmed HE still wrote `alp_ulog_partition`.
- HE's **master-side** firewall (FC8, which filters everything the HE core emits)
  *is* OEM-programmable through the ATOC device config. The proven policy is two
  regions: an allow-all region (`any_master`, `0x0`, 4 GB, `acc=0xfff`) plus a
  higher-priority deny carve-out over the log window (`0x80090000`, 4 KB, HE master
  id 17, `acc=0x000`). Region priority is **highest-index-wins**, so the deny must
  sit at a higher `region_index` than the allow.
- Result: HE boots and runs normally, but a direct `flash_write` to the log window
  raises a **BusFault** (probe beacon `RESULT_FAULT` at `STAGE_WRITE`), and the SE
  read (`maintenance -opt getmramdata`) confirms the MRAM is unchanged.

Notes for anyone reproducing this:

- Provision the FC8 policy over the SE-UART (`app-write-mram -e APP` then `-p`), not
  J-Link Flow D: a bricking firewall config can't be cleared by Flow D because its
  MRAM loader runs its RAMCode on the HE core. The SE writes MRAM independently, so
  the SE-UART path both provisions and recovers.
- The deny carve-out also blocks the **HE debug AP** read of the window, so the
  SWD-based `read-update-log-proof.sh` verdict false-fails ("MRAM changed"). Verify
  with the SE read (`getmramdata`, master 0) instead, or keep the design's read path
  on a non-HE master. Tracked upstream (#111).

The full dual-core path is also proven end-to-end on E8 (2026-07-06): with the FC8
policy active (deny sized to the whole 64 KB `alp_ulog_partition`), the SES boots the
HP owner, HP releases HE, and the HE client runs `open`/`append`/`verify`/`count`/`get`
entirely over the MHU mailbox — HP served all five requests (`ALP_OK`) and wrote the
entry to the MRAM NVS store (confirmed by the SE `getmramdata` read), while HE never
touches the partition directly. So HE reports `HW_ENFORCED`, the log is written only by
the trusted owner, and HE's own direct write bus-faults. The remaining gaps are E4
parity (E8 = E4 + A32; expected but not yet E4-bench-proven) and the anti-rollback NV
counter — both tracked under #111.

A **single-core** variant of the same guarantee is proven separately in
`examples/aen/aen-tz-secure-log-probe`: on one M55, TrustZone-M (the SAU) marks the
log window Secure so a Non-Secure application store to it faults (AttributionUnit
Violation). That is the path for single-M55 SKUs with no second core to own the log.

The AEN flash helpers build app-only ATOC packages by default. That preserves
the board's existing DEVICE/firewall policy, which is the policy the proof is
meant to test. Only set `ALP_AEN_INCLUDE_DEVICE_CONFIG=yes` when you
intentionally want the package to replace the DEVICE config too; doing that
with a generic config can remove the very firewall rule you are trying to
prove.

The proof is a negative test: build the HE firewall-probe profile and let HE try
to write the MRAM log partition directly. A valid hardware-enforced board either
rejects that write, blocks HE from reading the partition back, or raises a CPU
fault during the write. The bench helper captures the first 16 bytes before the
probe runs and compares them over SWD after the probe. If the bytes change, the
firewall is not active and the `HW_ENFORCED` profile must not be used.

The log is append-only and never wraps: when the partition is full,
`alp_update_log_append()` returns `ALP_ERR_NOMEM` and the existing chain
stays intact and verifiable.

## Running the example

```
west twister -p native_sim/native/64 \
    -T examples/connectivity/firmware-update-log --inline-logs
```

A passing run prints (among other lines):

```
[update-log] assurance: SW_TAMPER_EVIDENT (software tier)
[update-log] storage: RAM fallback
[update-log] verify: OK
[update-log] 1 entry:
  #0  v=1.4.2  status=0  ts=1718000000
```

With the current public AEN fail-closed config, the firewall-proof latch is not
enabled, so the HE board prints:

```
[update-log] HW_ENFORCED required, but no secure owner/firewall-backed backend is active
```

If `CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED` is temporarily disabled only
to inspect the software MRAM tier, the storage line reports:

```
[update-log] storage: MRAM NVS (alp_ulog_partition)
```

The HP owner build prints:

```
[update-log] AEN HP owner: MRAM writer service starting
[update-log] owner storage: MRAM NVS (alp_ulog_partition)
```

Before enabling the proven profile, run the HE direct-write firewall probe. This
is destructive only when the firewall is missing: in that case HE changes the
first 16 bytes of `alp_ulog_partition` and the helper reports failure.

```
west build -p always \
    -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/firmware-update-log \
    -d build/firmware-update-log-he-probe \
    -- -DALP_AEN_UPDATE_LOG_FIREWALL_PROBE=ON

scripts/bench/aen/flash-update-log-firewall-probe.sh --package-only \
    build/firmware-update-log-he-probe

# To test a newly provisioned board-specific DEVICE/firewall policy, place that
# JSON under the SETOOLS build/config directory and include it deliberately:
ALP_AEN_INCLUDE_DEVICE_CONFIG=yes \
ALP_AEN_DEVICE_CONFIG_JSON=<board-specific-device-config.json> \
scripts/bench/aen/flash-update-log-firewall-probe.sh --package-only \
    build/firmware-update-log-he-probe

ALP_CONFIRM_DESTRUCTIVE_FLASH=yes \
scripts/bench/aen/flash-update-log-firewall-probe.sh \
    build/firmware-update-log-he-probe

ALP_AEN_INCLUDE_DEVICE_CONFIG=yes \
ALP_AEN_DEVICE_CONFIG_JSON=<board-specific-device-config.json> \
ALP_CONFIRM_DESTRUCTIVE_FLASH=yes \
scripts/bench/aen/flash-update-log-firewall-probe.sh \
    build/firmware-update-log-he-probe
```

The probe stamps an SRAM0 beacon at `0x02001080`. The read helper decodes that
beacon directly: a protected board prints
`firewall verdict: PASS - HE could not modify alp_ulog_partition`; a writable
partition prints `firewall verdict: FAIL - HE changed alp_ulog_partition ...`
and exits non-zero.

| Result word | Meaning |
|---|---|
| `2` | CPU fault occurred while HE was attempting the direct MRAM write. |
| `3` | Direct write returned, but HE could not install its attempted pattern. |
| `4` | HE changed `alp_ulog_partition`; hardware enforcement is not active. |
| `5` | Probe setup/read failed; fix the board/build before claiming enforcement. |
| `6` | HE could not read the partition back; the helper must use SWD and the pre-flash baseline to decide pass/fail. |

For the dual-core AEN package, use a two-entry ATOC: HP is `M55_HP`
`["load", "boot"]` at `0x50000000`, and HE is `M55_HE` `["load"]` at
`0x58000000`. HP then releases HE at runtime with the portable
`alp_mproc_boot_core()` path and serves HE's update-log requests over MHU.
The bench helper below builds that exact package. Keep the normal HE build
fail-closed. Use the `-DALP_AEN_UPDATE_LOG_FIREWALL_PROVEN=ON` build only after
the board has been provisioned so the MRAM log partition rejects HE writes.
`--package-only` validates the app-only ATOC without writing MRAM.

```
west build -p always \
    -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/firmware-update-log \
    -d build/firmware-update-log-he-proven \
    -- -DALP_AEN_UPDATE_LOG_FIREWALL_PROVEN=ON

scripts/bench/aen/flash-update-log-dual.sh --package-only \
    build/firmware-update-log-hp build/firmware-update-log-he-proven

ALP_CONFIRM_DESTRUCTIVE_FLASH=yes \
scripts/bench/aen/flash-update-log-dual.sh \
    build/firmware-update-log-hp build/firmware-update-log-he-proven
```

After a real flash, the helper also reads SRAM0 proof beacons over SWD:

| Address | Words |
|---|---|
| `0x02000060` | HP owner: `magic`, last status, last operation, served request count |
| `0x02001060` | HE client: `magic`, last operation, last sequence, last status |
| `0x02001080` | HE firewall probe: `magic`, result, stage, detail, fault PC/status, partition offset |

For a hardware-enforced run, the HE beacon's last status word must be `0`
(`ALP_OK`) after the owner has served the append/verify/count/get requests.
Use `scripts/bench/aen/read-update-log-proof.sh --expect-hw` to re-read those
beacons without reflashing. Use
`scripts/bench/aen/read-update-log-proof.sh --expect-firewall-probe` to re-read
and decode the direct-write firewall probe without reflashing.

On a board that enables the TF-M owner or the AEN M55 owner with the firewall
proof latch, the HE application's first line reports:

```
[update-log] assurance: HW_ENFORCED (secure tier)
```

## Design reference

Full threat model, wire format, and acceptance criteria for the hardware backend
are in:
`docs/superpowers/specs/2026-06-11-secure-update-audit-log-design.md`
