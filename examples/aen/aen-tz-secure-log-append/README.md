<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-tz-secure-log-append — single-core TrustZone-M enforced append

End-to-end proof that on **one** M55 the Non-Secure application can append to the
audit log **only through the Secure owner**, and cannot write the log store
directly. This is the single-M55 counterpart to the two-core HP-owner / HE-client
flow in `examples/connectivity/firmware-update-log`; the sibling
`aen-tz-secure-log-probe` proves only the "blocked" half, this one proves the
whole owner/client cycle.

## Model

The core boots Secure (as the SES hands it off). The Secure world is the **owner**
of the log store; the Non-Secure world is the untrusted **app**:

1. The owner configures the SAU + TGU: a Non-Secure chunk of DTCM for the app
   (stub + stack + a request mailbox), with the log store left Secure.
2. The app (Non-Secure) writes an append **request** into the shared mailbox and
   returns to the owner — using only the Secure→Non-Secure call direction
   (`cmse_nonsecure_call`, which returns via `BX LR`). It never calls back into
   Secure, so no NS-callable (NSC) veneer is needed — Alif silicon does not
   attribute an NSC window for a custom region.
3. The owner (Secure) reads the request and performs the privileged log write the
   app itself is not allowed to do.
4. The app then attempts a **direct** store to the Secure log store — which raises
   an AttributionUnit Violation.

Ownership is temporal: the owner runs, hands control to the app, and services the
app's request on the return — the single-core equivalent of the two-core mailbox.

## Pass criterion (bench)

Read the SRAM0 beacon at `0x02001100`:

| Word | Meaning | Pass value |
|---|---|---|
| 0 | magic | `0x545A4C41` ("TZLA") |
| 1 | append count | `1` — the owner appended on the app's behalf |
| 2 | stored value | the app's value (`0xC0DE1234`) landed in the Secure store |
| 3 | result | `4` — the app's direct store faulted |
| 4 | `SCB->SFSR` | `0x48` = `AUVIOL` + `SFARVALID` |
| 5 | `SCB->SFAR` | address of the Secure log store |

Silicon-proven on E8 (`e1m-aen-evk-01`, 2026-07-06), on both the Flow C RAM-run and
the Flow D SES-boot path: `count=1`, `value=0xC0DE1234`, `result=4`, `SFSR=0x48`.
Because E4 is E8 with only the A32 cluster removed (identical M55/SAU/TGU/SE), this
holds on E4 unchanged.

## Running

```
# Non-destructive (ITCM-linked, RAM-run):
scripts/bench/aen/build.sh   $PWD/examples/aen/aen-tz-secure-log-append
scripts/bench/aen/ram-run.sh "$BENCH_ROOT/build/aen-tz-secure-log-append"
# then read 0x02001100 over SWD (table above)

# Or SES-boot it via Flow D: drop boards/*.overlay, add
# CONFIG_USE_DT_CODE_PARTITION=y, then scripts/bench/aen/flash-jlink-mramxip.sh.
```

Anti-rollback across a full reflash still needs a monotonic NV counter (#111).
