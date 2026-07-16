# 0013. TF-M trust boundary on AEN: TrustZone-M split on M55-HP

Status: Accepted
Date: 2026-05-20
Deciders: alpCaner

## Context

ADR 0010 cleared the heterogeneous-OS orchestration question (Zephyr +
Yocto as peers driven from `board.yaml` `cores:`).  That ADR left one
sub-decision deferred: **where does the TF-M secure partition live on
the AEN-family SoMs that carry two Cortex-M55 cores (HP + HE)?**

Two patterns are consistent with the ADR-0010 framing:

| Option | Boundary | Cost |
|---|---|---|
| **A. TrustZone-M on M55-HP** | Armv8-M security extension splits the M55-HP into a secure half (TF-M) and a non-secure half (Zephyr app).  Standard Cortex-M pattern; the secure and non-secure code share the same MPU + bus + interrupts. | Zero extra IPC; ~4 KB SAU + IDAU footprint; TF-M secure-side runs in the gap between non-secure ISRs. |
| **B. Dedicated M55-HE for TF-M** | M55-HE runs TF-M as its only workload; M55-HP runs Zephyr + the customer app; the two halves talk over the SoM's `mailbox:` controller using the same RPMsg endpoint scheme as ADR 0010's cross-core IPC. | Cross-core latency on every PSA call (~1-5 µs round-trip on Alif's HSEM); M55-HE consumed by a workload that can't accept compute offload; OPTIGA bridge driver gets a 2-hop path. |

The v0.6 `security.psa:` schema block (board.yaml flatten landed in
PRs #3 / #4 / #5) does not encode this distinction — the schema
exposes `tfm: true|false` and `attestation_root:`, but says nothing
about which core hosts TF-M.  The orchestrator's emit pipeline must
pick one; the choice has to be the same across every AEN-family SKU
to keep the secure-side image set portable per ADR 0011.

## Decision

**TrustZone-M split on M55-HP (Option A).**  The TF-M secure partition
runs on the same Cortex-M55-HP core as the non-secure Zephyr app, with
the Armv8-M security extension drawing the boundary.  M55-HE stays
available for compute / inference offload via `<alp/mproc.h>` and
`<alp/inference.h>` exactly as on AEN SKUs that don't enable TF-M.

The orchestrator emits the TF-M sysbuild child image at
`build/sysbuild/tfm/tfm.conf` when `security.psa.tfm: true`.  No
per-core routing field is added to `security.psa:`; the M55-HP
placement is implicit and documented at the schema, board-config doc,
and security-audit-plan layers.

## Consequences

### Positive

- **No extra IPC cost.**  PSA calls land in the same core's secure
  state via the SAU; round-trip is a function-call boundary, not a
  mailbox interrupt.
- **M55-HE stays a compute peer.**  Customers running inference on
  the HE core via `<alp/inference.h>` see no regression when they
  enable `security.psa.tfm: true` — the secure stack doesn't compete
  for HE cycles.
- **Standard pattern.**  Every Armv8-M reference flow (Zephyr's
  upstream TF-M integration, Alif's HAL examples, the NCS sample set)
  uses TrustZone-M on the application core.  Diverging from that
  would mean re-validating every secure-side example against the
  cross-core flow.
- **OPTIGA bridge stays 1-hop.**  The PSA <-> OPTIGA bridge driver at
  `chips/optiga_trust_m/optiga_trust_m.c` calls into OPTIGA via the
  M55-HP's I2C peripheral directly from the secure side; no cross-
  core RPC.

### Negative

- **Secure-side bugs can block the non-secure app.**  A fault in the
  TF-M partition halts M55-HP entirely, taking the customer app down
  with it.  Option B would have isolated the failure domains.
  Mitigated by the PSA Crypto + TF-M code being upstream, audited
  (per `docs/security-audit-plan.md`), and very small relative to
  the customer app surface.
- **MPU + SAU footprint on M55-HP.**  ~4 KB of SRAM lost to the
  security-attribute unit tables and a small fixed slice of the MPU
  region budget.  Negligible against the 13 MB SRAM tier of AEN E7 /
  E8; tighter on AEN E3 / E4 where SRAM is 8 MB but still well under
  1 %.

### Neutral

- The schema stays unchanged from the v0.6 land: no per-core routing
  field is added to `security.psa:`.  If a future SKU adopts Option
  B (e.g. an Alif part with a dedicated secure-only M-class peer),
  the schema can extend with a `tfm_core:` field — additive,
  default-on-HP, no migration burden on existing board.yaml files.

## Alternatives considered

**B. Dedicated M55-HE for TF-M (rejected above).**  Higher PSA call
cost, M55-HE consumed by a non-compute workload, OPTIGA bridge gains
a hop, and the secure-side image set diverges from upstream TF-M
samples that assume Option A.  The fault-isolation gain doesn't
justify the cross-cutting cost.

## References

- [ADR 0010](0010-heterogeneous-os-orchestration.md) — the broader
  cross-core trust-boundary framing this ADR refines.
- [`docs/security-audit-plan.md`](../security-audit-plan.md) — audit
  scope including the TF-M build flow.
- [`docs/board-config-features.md`](../board-config-features.md) §PSA Crypto + TF-M —
  the customer-facing `security.psa:` reference.
- [Armv8-M Security Extension](https://developer.arm.com/documentation/100690/0201/Memory-protection-unit-MPU--and-Security-Attribution-Unit--SAU--) —
  upstream reference for the TrustZone-M split.
- [Trusted Firmware-M](https://www.trustedfirmware.org/projects/tf-m/) —
  upstream TF-M project the sysbuild child image consumes.
