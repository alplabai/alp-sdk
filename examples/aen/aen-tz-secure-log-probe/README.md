<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-tz-secure-log-probe — single-core TrustZone-M enforcement of the MRAM log

A negative proof that on **one** M55 (no second core, no SES firewall device
config) the application can be hardware-blocked from writing the MRAM audit-log
window — using ARMv8-M TrustZone (the SAU + Alif TGU).

This is the single-M55 counterpart to the two-core route in
`examples/connectivity/firmware-update-log` (there HP owns the MRAM log and the
HE **master-side firewall FC8** blocks HE; here one core's own Secure/Non-Secure
split does the same job). It matters for single-M55 SKUs (e.g. E1/E1C) that have
no second core to offload the log writer to.

## Mechanism

The SES hands the M55 to the app in **Secure** state. The probe (Secure):

1. Enables SecureFault reporting, then does a Secure store to a scratch word to
   show Secure access works.
2. Configures the **SAU** (Security Attribution Unit) + **TGU** (Alif TrustZone
   Guard Unit) to carve a Non-Secure chunk out of DTCM. The MRAM log window
   (`0x80090000`) is left **Secure** (the SAU default: unconfigured memory is
   Secure).
3. Copies a tiny Non-Secure stub (`str r0,[r0]; b .`) into the NS chunk, sets
   `MSP_NS`, and calls it via a `cmse_nonsecure_call` (BLXNS) — passing the log
   window as the store target.
4. The stub, now running Non-Secure, stores to the Secure log window. That raises
   an **AttributionUnit Violation** — core-local, before the bus/firewall. A
   Non-Secure application cannot lift it because the SAU/TGU registers are
   Secure-only.

## Pass criterion (bench)

Read the SRAM0 beacon at `0x02001100` over SWD:

| Word | Meaning | Pass value |
|---|---|---|
| 0 | magic | `0x545A4C50` ("TZLP") |
| 1 | result | `0x4` = fault raised during the NS store |
| 2 | stage | `0x3` = fault happened in the NS-call stage |
| 4 | `SCB->SFSR` | bit3 `AUVIOL` + bit6 `SFARVALID` set (e.g. `0x48`) |
| 5 | `SCB->SFAR` | `0x80090000` — the faulting address is the log window |

Silicon-proven on E8 (`e1m-aen-evk-01`, 2026-07-06): `SFSR=0x48`, `SFAR=0x80090000`.
`AUVIOL` (not `INVEP`) + the log-window `SFAR` + a set `MSP_NS` together prove the
core was genuinely Non-Secure and its write to the Secure log window was rejected.

## Running (non-destructive — Flow C RAM-run)

The probe is ITCM-linked, so it runs from a J-Link RAM load and never touches
MRAM (the canonical slot0 image is preserved):

```
scripts/bench/aen/build.sh   $PWD/examples/aen/aen-tz-secure-log-probe
scripts/bench/aen/ram-run.sh "$BENCH_ROOT/build/aen-tz-secure-log-probe"
# then read the beacon at 0x02001100 over SWD (see the table above)
```

## What it does not do

This proves the app is *blocked* from the log window. A usable feature also needs
the write *path*: a Secure partition that owns the MRAM write, reached from the
Non-Secure app through an SG/NSC veneer (the TF-M model) — the single-core mirror
of the two-core HP-owner / HE-client mailbox. And, as with every tier here,
anti-rollback across a full reflash still needs a monotonic NV counter (#111).
