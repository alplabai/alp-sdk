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

## What the demo shows

The example runs in two acts so it answers the question *"can old entries be
modified or deleted -- and can you prove it?"* rather than just asserting it.

**Act 1 -- the API a customer writes.** Opens the log, records a realistic
field lifecycle (ship `1.4.0`, try `1.5.0` which fails verification, roll back,
then succeed with `1.5.1`), prints the assurance level, and verifies the chain.
This is the entire portable surface: the backend and the assurance level are
auto-selected at boot; the app never names a vendor mechanism.

**Act 2 -- the attacker.** Drops below the public API to the raw byte store and
hands an attacker direct write access to the log memory -- which on the Alif
E4/E8 is exactly what the application Cortex-M55 has (it writes MRAM directly
over the bus). It then performs four attacks on a healthy log and shows
`verify()` catches each one:

| Attack | Verdict |
|---|---|
| Modify a historical entry's bytes | `CHAIN_BROKEN` (names the offending seq) |
| Delete the newest entry | `TRUNCATED` |
| Roll the store back to an older snapshot | `ROLLED_BACK` |
| Reorder two entries | `CHAIN_BROKEN` |

The takeaway: **today** (software tier) tamper is *detected*; **with the E4
kit** the same API reports `HW_ENFORCED` and the SE firewall removes the
attacker's write access, so tamper becomes *prevented*. Tracking: #262
(durable store), #263 (authenticated inputs), #111 (hardware backend).

## Running the example

On native_sim (no hardware -- runs on your laptop, ideal for a live demo):

```
west twister -p native_sim/native/64 \
    -T examples/connectivity/firmware-update-log --inline-logs
```

A passing run prints (among other lines):

```
=== ACT 1: the API a customer writes (portable, auto-selected backend) ===
[update-log] assurance: SW_TAMPER_EVIDENT (software tier)
[update-log] verify: OK
[update-log] 5 entries:
  #0  v=1.4.0   status=0  ts=1718000000
  #1  v=1.5.0   status=3  ts=1718600000
  #2  v=1.5.0   status=1  ts=1718600060
  #3  v=1.4.0   status=2  ts=1718600120
  #4  v=1.5.1   status=0  ts=1719200000

=== ACT 2: attacker with raw access to the log bytes ===
  baseline (untampered)        -> verify: OK  [DETECTED]
  modify entry #1              -> verify: CHAIN_BROKEN (seq 2)  [DETECTED]
  delete newest entry         -> verify: TRUNCATED (seq 3)  [DETECTED]
  roll store back             -> verify: ROLLED_BACK  [DETECTED]
  reorder entries #0/#1       -> verify: CHAIN_BROKEN (seq 0)  [DETECTED]
```

## Design reference

Full threat model, wire format, and acceptance criteria for the hardware
backend are in:
`docs/superpowers/specs/2026-06-11-secure-update-audit-log-design.md`
