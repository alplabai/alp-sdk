# 0030. E1M-AEN SE firmware: SERAM and the services library are one matched pair, tracked at Alif's latest

Status: Accepted
Date: 2026-08-28
Deciders: alpCaner

## Context

The Secure Enclave on an Alif Ensemble E8 runs a firmware image Alif call
**SERAM**. An application reaches it through a **services library** — the
hal_alif `se_services` client this SDK links, pinned via `hal_alif` in
`west.yml`. The two halves speak a private protocol across the MHUv2 mailbox
pair, and until now this repo treated them as independent: SERAM was whatever a
module happened to be programmed with, the services library was whatever the
`hal_alif` pin brought in, and nothing stated a relationship between them.

Alif Semiconductor, 2026-08-28, answering our escalation on a customer's AE822
(#1700):

> there is an API break between SERAM v106 and v109 for E8 devices. v106 is a
> really early version for E8 platform, and you definitely need to update SERAM
> on your HWs to a newer version (v110 is recommended). It works ok with also
> with services library v109. General guideline is that you should always use a
> matching SERAM and services library.

An API break is not a bug with a symptom. Across one, the two halves disagree
about the shape of what they are exchanging, and **any** behaviour is
permissible — a clean error, a wrong answer, or hardware left in a state neither
side intended. The customer's module, running SERAM **1.106.2** against a
services library from SETOOLS **1.109**, stops HFXTAL and unlocks the PLL on its
first SE service request, dropping the M55-HP from 400 MHz to 76.8 MHz. Alif have
not said the break causes that, and it does not matter for this decision: the
pairing is unsupported either way, nothing observed on top of it is evidence
about anything, and the next mismatched module can fail differently.

This is a SoM-vendor problem, not a customer problem. Our customers buy a module
from us; discovering a firmware pairing rule by hitting undefined behaviour is
not something we can leave to each of them in turn.

## Decision

1. **SERAM and the services library are one versioned unit.** They move
   together. Neither is bumped alone, and a change to either is a change to the
   pair.

2. **E1M-AEN modules track Alif's latest released SERAM.** The current baseline
   and floor is **v110**, Alif's own recommendation, and the version our
   reference board runs (`SES A0 v1.110.0 Mar 4 2026`). "Latest" is the standing
   position, not v110 specifically — when Alif ship a newer SERAM we move to it
   and re-pin the services library alongside.

3. **A module below the floor is unsupported and untriageable.** Before any
   other investigation on an AEN SE fault, read the running version with
   `se_service_get_se_revision()` and clear a mismatch. No bug report against a
   mismatched pair is actionable, and no workaround built on one is safe to ship
   — including a workaround that appears to work.

4. **We state the baseline to customers rather than waiting to be asked.** The
   supported pairing is published in `docs/aen-se-services.md` §0.1 and gated in
   `docs/aen-provisioning.md` ahead of the SE-UART section, so a module owner
   meets it while provisioning rather than while debugging.

## Consequences

- The SERAM version becomes a thing we own and check, not an incidental property
  of a module. Bench boards get verified against the floor; a board below it is
  updated before it is trusted to produce evidence.
- Updating a fielded module is an SE-UART **System Package update** with
  SETOOLS — the same channel and tooling provisioning already uses, so this adds
  a step to an existing procedure rather than a new one.
- A `hal_alif` bump is no longer a routine dependency bump. It moves the services
  library, so it moves the required SERAM floor with it, and the pair has to be
  re-stated.
- **The services-library half is already at latest.** `hal_alif v2.3.0`, the
  `west.yml` pin, is the newest `hal_alif` release tag (checked 2026-08-28;
  `zas-v1.3-rc1` and the `zas-*` line are a separate series, not a successor).
  So this decision asks nothing of the SDK pin today — the work is entirely on
  the module side.
- **Open, and blocking a precise floor:** the pin does not reveal the answer.
  hal_alif versions its services library independently of SETOOLS —
  `se_services/include/services_lib_protocol.h` at `v2.3.0` declares
  `SE_SERVICES_VERSION_STRING "0.50.10"`, a number with no stated relation to
  SERAM v106/v109/v110 — and Alif publish no mapping between the two schemes.
  v110 is therefore taken as the baseline on the strength of Alif's
  recommendation and the reference board, not on a published correspondence.
  Tracked with the rest of the escalation on #1700.

## Alternatives considered

- **Pin a known-good older pair and stay there.** Rejected: the older pair
  available to us is v106, which sits on the wrong side of the break and which
  Alif describe as "a really early version for E8 platform". Standing still means
  standing on the version they are telling us to leave.
- **Leave the pairing to each module owner.** Rejected — that is the status quo
  that produced #1700. The rule is invisible from inside an application, and the
  failure it produces does not name itself.
- **Say nothing until Alif confirm a causal chain for #1700.** Rejected: waiting
  on a causal answer to act on an API break inverts the risk. The pairing is
  unsupported today, and a customer running one is exposed today.
