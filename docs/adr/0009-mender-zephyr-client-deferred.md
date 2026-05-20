# 0009. Mender Zephyr client deferred to v1.1; secure OTA on Zephyr cuts from v0.4

Status: Accepted
Date: 2026-05-14

## Context

The v0.4 release plan (`VERSIONS.md`) included **secure OTA on
AEN-Zephyr** as a deliverable.  The Yocto side runs Mender
(via meta-mender on V2N-Yocto + i.MX 93-Yocto); the Zephyr
side needs a peer OTA client to deliver signed updates to the
MCUboot secondary slot.

Two candidates for the Zephyr-side client:

1. **`mender-mcu-client`** -- Mender's official Cortex-M client.
   Pros: same Mender server back-end as the Yocto side ⇒ one
   OTA infrastructure across the fleet.  Cons: ties the SDK to
   Mender's client roadmap; client itself is younger + has
   thinner upstream Zephyr integration today.
2. **Eclipse Hawkbit** + Zephyr's upstream
   `subsys/mgmt/hawkbit/` -- the Hawkbit subsystem already in
   upstream Zephyr.  Pros: lighter-weight on the Zephyr side;
   decoupled from a single vendor.  Cons: V2N-Yocto stays on
   Mender, so the SDK has to support **two** OTA back-ends
   long-term.

Either choice locks in non-trivial back-end + UI infrastructure
on the operations side.  v0.4's other deliverables (Yocto
first-class, secure boot on Zephyr, MCUboot integration) are
all on schedule; the Mender-vs-Hawkbit decision is the long
pole.

## Decision

**Defer the AEN-Zephyr OTA client decision to v1.1.**  v0.4
ships:

- ✅ **Secure boot on AEN-Zephyr** -- MCUboot + ECDSA-P256 +
  swap-using-scratch (`zephyr/sysbuild/aen/sysbuild.conf`).
- ✅ **Secure OTA on V2N-Yocto + i.MX 93-Yocto** -- Mender via
  meta-mender (`meta-alp-sdk/conf/distro/include/mender.inc`).
- 📋 **Secure OTA on AEN-Zephyr** -- deferred.  The
  `test-plan.md` row for v0.4 secure-OTA-on-Zephyr moves to
  v1.1.

This means a v0.4 / v1.0 customer running on AEN-Zephyr has
**signed images that boot but no in-field update path**.
That's a real product gap, called out explicitly in the
release notes.  Workarounds for the v0.4 → v1.0 window:

- Physical-access flash (J-Link / OpenOCD against MCUboot's
  secondary slot).
- Board-side OTA fronted by a Linux companion (e.g. a V2N
  SoM coordinating the AEN-Zephyr update through the bridge).

## Consequences

### Positive

- v0.4 tags on schedule.  Yocto first-class + secure boot on
  Zephyr are real customer features.
- The Mender-vs-Hawkbit decision lands when there's real
  customer pull, not on a release deadline.
- v1.0 ships with the deferred-feature surface clearly marked
  rather than landing a rushed integration that ages badly.

### Negative

- AEN-Zephyr customers in the v0.4..v1.0 window have no
  in-field update story.  This is a real gap.  Workarounds
  documented in `docs/ota.md`.
- The v1.0 LTS commits to 24 months of support without
  knowing which Zephyr OTA client we'll back -- the v1.1
  decision becomes a soft API shift inside the LTS window.

### Neutral

- The `<alp/...>` public surface doesn't move.  Both
  `mender-mcu-client` + Hawkbit slot in behind an OS-level
  service rather than a new SDK header, so the decision
  affects backend wiring + `board.yaml` opt-in flags, not the
  customer-facing C ABI.

## Alternatives considered

- **Pick Mender now, ship a thin client wrapping
  `mender-mcu-client`.**  Rejected: `mender-mcu-client` upstream
  Zephyr integration is still in flux as of 2026-05; landing a
  thin wrapper that we then have to retract if Mender changes
  the integration surface is worse than waiting.
- **Pick Hawkbit now.**  Rejected: V2N-Yocto stays on Mender,
  so we'd lock in two OTA back-ends from day one.  Real-
  customer pull may make this the right call later, but
  pre-emptive lock-in is premature.
- **Drop Zephyr secure boot too, defer everything.**  Rejected:
  signed boot is the load-bearing v0.4 promise; OTA without
  signed boot is meaningless, but signed boot without OTA is
  still a real deliverable (factory-flashed firmware that's
  tamper-resistant).

## See also

- `VERSIONS.md` v0.4 + v1.0 sections — what shipped + the
  v1.1 deferral.
- [`docs/ota.md`](../ota.md) — Yocto-side OTA design.
- [`docs/ota-device-contract.md`](../ota-device-contract.md) — device-side
  contract (Yocto only as of v0.4).
- [`docs/test-plan.md`](../test-plan.md) — the Zephyr-secure-OTA
  row moves from v0.4 to v1.1.
- [memory: pending-hw-configs / mender-decision-deferred] —
  captured during the V1.0 readiness planning 2026-05-14.
