# firmware-update-log

Portable, tamper-evident firmware-update audit log using `<alp/update_log.h>`.

## One API across SoMs

The same application code works on every Alp SoM. The backend is selected
automatically at boot. On native_sim (and any SoM without a dedicated secure
element) the software tier runs. On SoMs with TF-M and a hardware monotonic
counter the same API is hardware-enforced — no application change required.

## Assurance levels

| Level | Value | What you get |
|---|---|---|
| `SW_TAMPER_EVIDENT` | `0` | SHA-256 hash-chain + monotonic counter (NVS-persisted where the board provides an `alp_ulog_partition`; RAM otherwise). Detects mutation, truncation, rollback, and reorder of historical entries. App-cooperative: a process with write access to the store can forge entries. Use for audit trails where cooperative tamper-evidence is sufficient. |
| `HW_ENFORCED` | `1` | TF-M Protected Storage (Secure Processing Environment, unreachable from NS) + a hardware non-decrementable monotonic counter (PSA NV or OPTIGA). An attacker with full OS access cannot forge or roll back entries without breaking TF-M isolation. Present where the secure backend is registered. |

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
tier (TF-M isolation + hardware counter, issue #111).

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
[update-log] verify: OK
[update-log] 1 entry:
  #0  v=1.4.2  status=0  ts=1718000000
```

## Design reference

Full threat model, wire format, and acceptance criteria for the hardware
backend are in:
`docs/superpowers/specs/2026-06-11-secure-update-audit-log-design.md`
