# External security-audit engagement plan

> **Status:** open — pending firm selection.  Threat model, SBOM
> template, and SLSA L3 release provenance (the in-repo Pillar 8
> deliverables) all landed in §C.9 / §C.18 / §C.27.  This doc
> describes the **third-party audit** that's the remaining
> external-party item.

## Scope

The v1.0 audit covers:

1. **Threat-model verification.**  The auditor reviews
   [`docs/threat-model.md`](threat-model.md) against the
   shipping codebase and flags missed assumptions or
   under-modelled attack surfaces.
2. **Source-level review of the security surfaces.**
   Specifically `<alp/security.h>` (AEAD + TRNG +
   PSA-Crypto seam), `<alp/iot.h>` TLS wrapper,
   `<alp/storage.h>` inline-AES configuration path,
   and the OPTIGA Trust M chip driver.  Authentication +
   provisioning flows in `chips/optiga_trust_m/`.
3. **MCUboot integration audit.**  Signing-service contract
   in [`docs/secure-boot.md`](secure-boot.md); the SDK's
   ECDSA-P256 key handling + the rollback-prevention
   mechanism.
4. **OTA path penetration test.**  Mender deployment
   contract in [`docs/ota-device-contract.md`](ota-device-contract.md).
   Auditor attempts: signed-image bypass, downgrade
   attacks, partial-write recovery, TLS-stripped probe of
   the Mender connection.
5. **Fuzz-corpus inheritance.**  Auditor reviews
   `tests/fuzz/` and either inherits a corpus or stages
   their own; findings come back as new corpus entries.

## TF-M trust boundary (cross-core)

The PSA Crypto secure partition runs on the same M55-HP core as the
non-secure app via the Armv8-M security extension (TrustZone-M split),
**not** on a dedicated M55-HE core.  This is the standard Cortex-M
pattern, costs no extra IPC, and keeps M55-HE free for compute /
inference offload.  The decision is captured in
[`docs/adr/0013-tfm-boundary-m55-hp-trustzone.md`](adr/0013-tfm-boundary-m55-hp-trustzone.md);
see ADR 0010 for the broader cross-core trust-boundary discussion.

The auditor reviews the TF-M build flow (`board.yaml`
`security.psa.tfm: true` -> orchestrator emits
`build/sysbuild/tfm/tfm.conf` -> sysbuild builds the TF-M secure
image as a child image) and the PSA <-> OPTIGA Trust M bridge driver
at `src/security/optiga_trust_m_bridge.c`.

## Out of scope (separate engagements)

- **Vendor-side silicon vulnerabilities** -- those belong
  to the SoC vendor (Renesas / Alif / NXP) and ride their
  own audit cycles.  Vendor security advisories that
  affect the SDK propagate through
  [`docs/security-advisories.md`](security-advisories.md).
- **Customer application code** -- variants of
  `examples/connectivity/production-deployment/` that customers ship to
  production carry their own audit scope.
- **alp-studio** -- separate repo, separate audit cycle.

## Firm-selection criteria

We're sourcing the audit firm against these criteria:

1. Track record on embedded Linux + Zephyr -- not just
   web / cloud security.
2. Familiarity with MCUboot + PSA Crypto + TLS pinning
   patterns.
3. Willingness to inherit our existing threat model
   instead of re-deriving from scratch (saves ~$30k of
   engagement time).
4. Public-disclosure norms aligned with the
   [`docs/security-advisories.md`](security-advisories.md)
   90-day embargo policy.

Shortlist (ranked by initial conversation):
- **Trail of Bits** -- strong Zephyr + MCUboot track record;
  expensive but produces the most actionable reports.
- **NCC Group** -- specialised in embedded crypto reviews;
  good fit for the OPTIGA + AEAD surfaces.
- **Doyensec** -- strong on TLS / IoT cloud connections;
  weaker on baremetal Zephyr.

## Timeline

- **2026-Q2** -- firm selected, statement-of-work signed.
- **2026-Q3 (8 weeks)** -- audit engagement, including
  weekly check-ins with the SDK maintainer.
- **2026-Q3 close** -- final report.  Findings under
  90-day embargo per the public-disclosure policy.
- **2026-Q4 (4 weeks)** -- remediation work.  Each
  finding tracked as an Issue against this repo with the
  `security-audit-finding` label.
- **2026-Q4 close** -- public disclosure + addendum to
  `docs/security-advisories.md`.

## Budget

Estimated $80–120k for a thorough 6–8 week engagement.
Funding is line-item in the v1.0 launch budget.

## What lands in-repo after the audit

- `docs/audit-2026-q3/report.pdf` -- the final report
  (after the embargo period expires).
- `docs/audit-2026-q3/findings-summary.md` -- one-page
  summary with each finding's status (fixed / accepted-
  risk / closed-no-action).
- `tests/fuzz/corpus/*/` -- corpus entries from the
  auditor's fuzz runs.
- CHANGELOG entry under "### Security (2026-Q4)" listing
  every CVE we mint plus links to the per-finding fix
  commits.

## How this gates v1.0

The v1.0 tag does NOT block on completion of the audit.
Per the release policy, v1.0 ships with the in-repo
security work (threat model, SBOM, SLSA L3) and the audit
findings land as a v1.0.x point release per
[`docs/release-policy.md`](release-policy.md).  This lets
customers start integrating against a stable surface
while the audit runs in parallel.

If a finding is severe enough to break the v1.0 ABI, the
release policy's pre-1.0 ABI-breaking provision (§A.6
convention) applies until we ship v1.0; post-1.0 the
finding ships as a v2.0 candidate via the MAJOR-bump
rules in `docs/release-policy.md`.
