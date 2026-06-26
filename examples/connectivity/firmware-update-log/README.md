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
| `SW_TAMPER_EVIDENT` | `0` | SHA-256 hash-chain + RAM monotonic counter. Detects mutation, truncation, rollback, and reorder of historical entries. App-cooperative: a process with write access to the store can forge entries. Use for audit trails where cooperative tamper-evidence is sufficient. |
| `HW_ENFORCED` | `1` | TF-M Protected Storage (Secure Processing Environment, unreachable from NS) + a hardware non-decrementable monotonic counter (PSA NV or OPTIGA). An attacker with full OS access cannot forge or roll back entries without breaking TF-M isolation. Present where the secure backend is registered. |

Call `alp_update_log_assurance()` to query which level is active at runtime.

## Slice-1 scope

The software store in this slice is **in-RAM**: the log does not survive a
reboot. Durable persistence via Zephyr Settings/NVS (so the chain survives
power-cycles) is the named follow-up before the software tier is production.
The engine contract and assurance level are unchanged by that swap — only the
`alp_secure_store_if` implementation gains persistence; the API is identical.

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
