# 0033. The CC3501E wire protocol is versioned MAJOR.MINOR, and features are discovered by capability

Status: Proposed
Date: 2026-09-03
Deciders: alpCaner

## Context

The CC3501E wire protocol went from **v5 to v9 in a single week** (2026-08-27 →
2026-09-03). Every one of those bumps forced customers to reflash *both* halves
in lockstep, because the host refuses the link outright on any mismatch
(`chips/cc3501e/cc3501e_core.c:234`, `if (fw_version !=
ALP_CC3501E_PROTOCOL_VERSION)` → `ALP_ERR_VERSION`, and it clears
`initialised` so every later call fails).

That refusal is correct for *some* of those bumps and wrong for others. Sorting
the actual history by the only question that matters — **would an old host be
MISREAD by the new firmware?**:

| Bump | What changed | Old host misread? | Should have been |
|---|---|---|---|
| v6 | SPI1 host-passthrough opcodes `0x55`..`0x57` | **No** — an old host never sends them | MINOR |
| v7 | `alp_cc3501e_sock_send_t::reserved` (offset 3) reinterpreted as a retry seq | **Yes** — an old host writes 0 there, which a v7 firmware reads as a valid seq and may answer from a stale cache | MAJOR |
| v8 | Frame-header flags bits 3..7 reinterpreted as a 5-bit retry seq | **Yes** — same shape, at the header instead of one struct | MAJOR |
| v9 | `SOCK_BIND` `0x25`, `SOCK_LISTEN` `0x26`, `EVT_SOCK_ACCEPTED` `0x2C`; optional interface byte on `WIFI_GET_IP` `0x17` (zero-length request keeps its pre-v9 meaning) | **No** — additive; an old host never sends the new opcodes and its zero-length `GET_IP` still means STA | MINOR |

**Two genuine breaking changes, presented as four.** The version number
over-reports breakage by 2×, and each over-report costs a customer a
coordinated reflash of a module that is otherwise working.

The root cause is that one integer does two jobs at once:

1. a **compatibility gate** — "is it safe for these two binaries to talk?", and
2. a **feature marker** — "does this firmware have listening sockets?"

Because the gate is exact equality, job 2 keeps forcing job 1 to fire. A purely
additive opcode cannot hurt anybody, and today it is indistinguishable from a
semantic reinterpretation that genuinely can.

There is a second, quieter problem. Customers are being handed the raw wire
integer as their compatibility story — `alp companion ver` prints
`CC3501E protocol v9`, and release notes quote it — when the number they can
actually act on is the firmware release SemVer (`firmware-version.txt`, e.g.
`0.6.0`). The wire integer is an internal contract between the host library and
the firmware; it leaked into the customer-facing surface because it was the
only version we printed.

## Decision

### 1. The wire version is MAJOR.MINOR, and only MAJOR gates the link

`ALP_CC3501E_PROTOCOL_MAJOR` and `ALP_CC3501E_PROTOCOL_MINOR` replace the single
`ALP_CC3501E_PROTOCOL_VERSION` integer. `CMD_GET_VERSION` (`0x01`) keeps its
2-byte LE reply and encodes `(MAJOR << 8) | MINOR`.

The rule for which one moves is a single question, and it is deliberately not a
judgement call about "how big" the change feels:

> **MAJOR** — an existing host, unchanged, would be **misread** by the new
> firmware, or would misread its replies. Reusing a previously-reserved byte or
> flag bit, changing a struct layout, changing framing, or changing the meaning
> of an existing field all qualify.
>
> **MINOR** — everything additive. New opcodes, new optional request fields
> whose absent form keeps its old meaning, new event types, new capability bits.

The host gate becomes:

- `major != ALP_CC3501E_PROTOCOL_MAJOR` → refuse, exactly as today
  (`ALP_ERR_VERSION`, `initialised` cleared). This is the safety property v7 and
  v8 needed and it is preserved unchanged.
- `major == 0` → the firmware predates this scheme (it is answering with a raw
  v1..v9 integer). Refuse, but with a *distinguishable* diagnostic: this is
  "your firmware is older than the scheme", not "your firmware is corrupt".
- otherwise → **connect**, and record the firmware's minor on the context. A
  firmware with a *lower* minor than the host simply lacks newer features; a
  firmware with a *higher* minor has features this host does not use.

Retroactive mapping, so the history above stays legible: v5 = `1.0`, v6 = `1.1`,
v7 = `2.0`, v8 = `3.0`, v9 = `3.1`. The current wire is therefore **3.1**, and
it is byte-identical to what shipped as "v9" — the scheme renames the contract,
it does not change a single frame.

### 2. Features are discovered by capability, not inferred from a version

`CMD_GET_CAPABILITIES` (`0x06`, the next free code in the meta group) returns a
bitmap of the opcode families the firmware **actually implements in this
build**. Host code asks "do you have listening sockets?" rather than "are you at
least 3.1?".

This matters beyond tidiness, because the version number cannot express what the
bitmap can: this firmware has real build variants. A build without
`CC3501E_WIFI` has the socket opcodes compiled as `NOTIMPL` stubs, and a build
without `CC3501E_BLE` has no BLE host at all. Both report the same wire version
as a full build while behaving completely differently. A capability bitmap
composed at compile time from those same switches reports the truth.

Consequence for the future: **an additive feature never breaks a link again.**
It bumps MINOR, sets a capability bit, and an old host neither knows nor cares.

### 3. The customer-facing version is the firmware SemVer

`firmware-version.txt` (e.g. `0.6.0`) is what release notes, support threads and
`prebuilt/CHANGELOG.md` quote. The wire version is an implementation detail of
the host↔firmware contract.

`alp companion ver` therefore prints both, with the release version first:

```
fw 0.6.0  (wire 3.1)
```

### 4. A MAJOR bump must be justified in writing, and CI enforces it

`scripts/check_protocol_version_policy.py` fails when
`ALP_CC3501E_PROTOCOL_MAJOR`.`MINOR` has no matching row in the
[Version ledger](#version-ledger) below (the machine-readable one this gate
actually parses -- not the prose table under Context, which is the historical
analysis, not the source of truth), and when the ledger's newest row disagrees
with the header. A row marked `MAJOR` must carry a "an old host would be
misread because …" sentence.

The gate exists because the failure mode it prevents is *social*, not technical:
bumping the number is the path of least resistance when you are unsure, and four
bumps in a week is what that looks like. Making a MAJOR bump cost a written
justification puts the friction where the decision is.

## Version ledger

Machine-readable, one row per released wire version -- `scripts/
check_protocol_version_policy.py` parses the single fenced block below, never
the prose table under [Context](#context) above (free-form, for humans). Row
format: `MAJOR.MINOR = MAJOR|MINOR = justification`.

A row marked `MAJOR` must state that an existing host would be **misread**;
that sentence is the entire test the gate applies, straight out of the rule in
Decision point 1. Rows must appear in strictly increasing MAJOR.MINOR order,
and the last row must always equal `ALP_CC3501E_PROTOCOL_MAJOR`.`MINOR` in
`include/alp/protocol/cc3501e.h`.

```
1.0 = MINOR = v5: retroactive baseline, the first wire version under this scheme; nothing precedes it to be misread
1.1 = MINOR = v6: added SPI1 host-passthrough opcodes 0x55..0x57 -- additive, an old host never sends them
2.0 = MAJOR = v7: alp_cc3501e_sock_send_t::reserved (offset 3) reinterpreted as a retry seq -- an old host writes 0 there, which a v7 firmware reads as a valid seq and may answer from a stale cache; an old host would be misread
3.0 = MAJOR = v8: frame-header flags bits 3..7 reinterpreted as a 5-bit retry seq -- same shape as v7, at the header instead of one struct; an old host would be misread the same way
3.1 = MINOR = v9: added SOCK_BIND/SOCK_LISTEN/EVT_SOCK_ACCEPTED, optional WIFI_GET_IP iface byte
```

## Consequences

**This costs one final breaking bump.** Moving from a raw integer to
`(MAJOR << 8) | MINOR` changes what `GET_VERSION` returns, so a v9 host and a
`3.1` firmware do not interoperate even though the frames are identical. That is
unavoidable — and it is the last one, which is the point. Doing it now, with one
customer on the protocol, is materially cheaper than doing it at ten.

**The refusal path gets weaker, on purpose.** A host that connects to a
higher-MINOR firmware is talking to a binary with opcodes it does not know. That
is safe precisely because MINOR is *defined* as "additive": the host never sends
what it does not know, and the firmware never spontaneously sends an event the
host did not enable. If a change cannot honour that, it is MAJOR by definition —
the rule is the safety argument.

**Capability bits are forever.** A bit, once assigned, cannot be reused for a
different feature; retiring a feature retires its bit. This is the same
discipline as the opcode space and has the same reason.

**The history table is load-bearing, not decoration.** The CI gate reads it. It
is also the honest record of why v6 and v9 need not have cost anyone a reflash,
which is the mistake this ADR exists to stop repeating.

## Alternatives considered

**Keep one integer, relax the gate to `>=`.** Rejected: it silently permits
exactly the v7/v8 case, where a *newer* firmware misreads an older host's
zero-filled reserved bits. The direction of the comparison is not the problem;
conflating two kinds of change is.

**Add a separate `CMD_GET_PROTOCOL` and leave `GET_VERSION` alone.** Rejected:
two sources of truth for one fact, and the repo's stated principle is that every
hardware or contract fact has exactly one machine-readable source. It would also
leave the misleading raw integer in place as the thing old tooling keeps
reading.

**Negotiate a version at attach (host proposes, firmware accepts).** Rejected as
premature: it buys nothing over MAJOR-gate + capabilities while there is a
single wire format, and it adds a round trip plus a state machine to a transport
whose failure modes we are still learning.

**Do nothing and batch wire changes into release windows.** Rejected as
insufficient on its own — batching reduces how *often* customers reflash without
fixing the fact that additive changes force a reflash at all. Worth doing
anyway, and orthogonal to this ADR.
