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
requests over MHU, the owner writes the MRAM log, and the Alif SE/device
firewall must block the application core from writing that MRAM partition.

## Assurance levels

| Level | Value | What you get |
|---|---|---|
| `SW_TAMPER_EVIDENT` | `0` | SHA-256 hash-chain + monotonic counter (NVS-persisted where the board provides an `alp_ulog_partition`; RAM otherwise). Detects mutation, truncation, rollback, and reorder of historical entries. App-cooperative: a process with write access to the store can forge entries. Use for audit trails where cooperative tamper-evidence is sufficient. |
| `HW_ENFORCED` | `1` | Trusted-owner store isolated from the application core. Normal application firmware cannot rewrite old entries when the owner and its storage are isolated by the platform. On TF-M this is a secure partition and protected storage. On AEN this is an M55 owner plus SE/device firewall-protected MRAM; a non-decrementable rollback anchor still depends on the board's provisioned secure counter or equivalent device policy. |

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

For the dual-core AEN package, use a two-entry ATOC: HP is `M55_HP`
`["load", "boot"]` at `0x50000000`, and HE is `M55_HE` `["load"]` at
`0x58000000`. HP then releases HE at runtime with the portable
`alp_mproc_boot_core()` path and serves HE's update-log requests over MHU.
The bench helper below builds that exact package. Keep the normal HE build
fail-closed. Use the `-DALP_AEN_UPDATE_LOG_FIREWALL_PROVEN=ON` build only after
the board has been provisioned so the MRAM log partition rejects HE writes.
`--package-only` validates the ATOC without writing MRAM.

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

For a hardware-enforced run, the HE beacon's last status word must be `0`
(`ALP_OK`) after the owner has served the append/verify/count/get requests.
Use `scripts/bench/aen/read-update-log-proof.sh --expect-hw` to re-read those
beacons without reflashing.

On a board that enables the TF-M owner or the AEN M55 owner with the firewall
proof latch, the HE application's first line reports:

```
[update-log] assurance: HW_ENFORCED (secure tier)
```

## Design reference

Full threat model, wire format, and acceptance criteria for the hardware backend
are in:
`docs/superpowers/specs/2026-06-11-secure-update-audit-log-design.md`
