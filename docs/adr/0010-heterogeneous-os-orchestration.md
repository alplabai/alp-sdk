# 0010. Heterogeneous OS orchestration: Zephyr + Yocto as peers, not alternatives

Status: Accepted
Date: 2026-05-15
Deciders: alpCaner

## Context

The SDK presents Zephyr and Yocto as mutually exclusive build targets
selected by a single `os:` field in `board.yaml`.  That framing is
architecturally wrong for the SoMs the SDK actually ships against:

- **E1M-V2N101** carries 4× Cortex-A55 plus 1× Cortex-M33 system-manager
  on the same RZ/V2N die.
- **E1M-AEN701** carries 2× Cortex-A32 plus 2× Cortex-M55 (HP + HE) on the
  same Alif Ensemble E7 die.
- **E1M-NX9101** carries 2× Cortex-A55 plus 1× Cortex-M33 on i.MX 93.

A real product on one of these SoMs needs Yocto on the A-cluster *and*
Zephyr on the M-class peer, talking over IPC.  The current schema forces
the customer to pick one and hand-implement the other half outside the
SDK.

The audit (full table in the design spec §1) found five most damning
locations where the either/or lives today:

| # | Location | Issue |
|---|---|---|
| 1 | `metadata/schemas/board-config-v1.schema.json:99-103` | `os:` is a single-string enum (`zephyr \| yocto \| baremetal`). |
| 2 | `examples/mproc-dual-os-yocto-zephyr/board.yaml` | Documented workaround: a 13-line comment apologising that the Yocto half lives in a separate bitbake recipe that does not consume the same config. |
| 3 | `include/alp/mproc.h:50-57` | Runtime API already knows about heterogeneous cores (`ALP_CORE_M55_HP/HE/A32_0/A32_1`) — the build can't actually produce both halves. |
| 4 | `metadata/socs/.../*.json` `cores[]` | SoC metadata already lists cores per silicon; nothing reads it for OS routing. |
| 5 | (no file) | No system-level manifest captures "which core runs which OS".  That mapping lives in prose in example READMEs. |

The SDK's value proposition is that the customer declares what they want
and the SDK fans out into per-vendor build systems.  The current
architecture abandons that proposition the moment heterogeneous compute
enters the picture.

## Decision

Drive every heterogeneous SoM from a single per-core mapping in
`board.yaml` v2.  Six axes:

| Axis | Decision |
|---|---|
| **Scope** | All heterogeneous SoMs (V2N, AEN E5..E8, iMX93, future silicon).  One generic mapping driven from `metadata/socs/.../cores[]` — no per-SoM special cases bleed into the schema. |
| **IPC** | OpenAMP / RPMsg + virtio as canonical.  `alp_mbox` / `alp_shmem` / `alp_hwsem` stay as low-level primitives; `<alp/rpc.h>` is the framed channel customers use — apps never type an endpoint ID or carve-out address by hand. |
| **CLI** | Extend `west alp-build` to fan out across cores.  New companions: `west alp-image`, `west alp-flash`, `west alp-clean`, `west alp-renode` — one entry point per stage of the build → bundle → flash → simulate pipeline. |
| **Schema** | Bump `board.yaml` to `schema_version: 2`.  Per-core `cores:` block mandatory; the `os:` enum removed.  No migration script — every in-repo `board.yaml` is rewritten in the same PR as the schema rev. |
| **CI** | PR-level: bitbake every A-cluster MACHINE, Zephyr-build every M-class slice, Renode dual-OS smoke test.  Twister stays as the Zephyr-only fast lane because the iteration loop on a single slice should not pay the orchestrator's overhead. |
| **Defaults** | Heterogeneous-by-default: every on-die programmable core in the SoM preset declares a sensible default OS + app.  A bare `som: { sku: E1M-V2N101 }` produces both A55=Yocto and M33-SM=Zephyr; opt-out is explicit (`os: off`). |

Full design at `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`.

## Consequences

### Positive

- **Dual-OS becomes table-stakes.**  Heterogeneous SoMs (V2N, AEN E5+,
  iMX93) ship out-of-the-box with both halves built from one declarative
  file.  The 13-line apology comment in the old
  `examples/mproc-dual-os-yocto-zephyr/board.yaml` is deleted; that
  example is renamed to `examples/rpmsg-v2n/` and becomes the reference
  for how the schema *expects* to be used.
- **One declarative file drives the whole system.**  The customer's
  `board.yaml` plus the SoM preset is the single source of truth.  Cross-
  core artefacts (RPMsg endpoint IDs, DT carve-outs, remoteproc firmware
  paths) are generated from the same input — no manual sync between the
  Linux DT and the Zephyr overlay.
- **Helper-MCU firmware joins the manifest.**  GD32 + CC3501E build
  outputs are registered in `system-manifest.yaml` so `west alp-flash`
  covers them automatically.  Today's customers hand-walk between the
  SDK's flash command and the helper-MCU vendor tools.
- **CI verifies the integration.**  `pr-renode-dual-os.yml` boots the V2N
  system manifest end-to-end and asserts the RPMsg handshake completes.
  The promise that "both halves work together" gates every PR rather
  than being a doc claim.
- **alp-studio gains a unified target.**  Studio codegen consumes the
  same per-core mapping the hand-written-firmware path uses.  Adding a
  visual block for a cross-core RPC call is a schema extension, not a
  re-architecture.

### Negative

- **v0.5 → v0.6 schema break.**  Every `board.yaml` in the repo (~32
  files) is rewritten in one PR.  No migration script — there are no
  shipping customers yet, and a migration script is dead code from day
  one (see "Alternatives considered" below).  External branches with
  v1 `board.yaml` files break.
- **PR CI gets slower.**  Bitbake adds a build path that, cold, runs in
  hours.  Mitigated by a self-hosted runner with persistent sstate-cache
  + ccache — steady-state warm rebuilds settle around 5 min.  Cold-build
  cost is a one-time investment when the runner is provisioned.
- **Per-SoM memory_map needs hand-curation.**  Each SoM preset declares
  the DDR / OCRAM / TCM regions and which cores see them.  AEN and iMX93
  presets have to land cleanly authored from the vendor reference
  manuals — defaults from V2N do not transfer.  The metadata PR includes
  a cross-check against the relevant datasheet section per row.
- **Learning curve for app developers.**  Writing a dual-app project
  means thinking in terms of a system topology, not a single firmware
  image.  Mitigated by the new
  [`docs/heterogeneous-builds.md`](../heterogeneous-builds.md) walkthrough
  + the `examples/rpmsg-v2n/` reference layout.

### Neutral

- The `<alp/...>` public surface gains `<alp/system_ipc.h>` (generated)
  and grows `<alp/rpc.h>` from its v0.3 declaration into a populated
  surface.  `<alp/mproc.h>` is unchanged — RPC sits on top of mailbox +
  shmem + hwsem rather than replacing them.
- Single-OS examples are unchanged in spirit.  A `board.yaml` that
  declares only one core works as a single-slice fan-out — same build
  experience as the v1 `os:` path, just routed through the orchestrator.
  AEN E3 / E4 (no A-class) customers see no regression.

## Alternatives considered

**A. V2N-only-first, generalise later.**  Ship a V2N-specific dual-OS
mode in v0.6, generalise the schema in v0.7 once AEN and iMX93 catch up.
Rejected because:

- It bakes V2N-shaped assumptions into the schema we'd have to rev anyway
  (V2N has exactly one M33; AEN has two M55s; iMX93 has one M33 of a
  different class — a V2N-shaped schema doesn't carry the AEN second-M55
  case cleanly).
- The metadata layer (`cores[]`) is already generic.  Restricting the
  schema to be less general than the metadata it consumes is purely
  cosmetic and creates two refactors instead of one.

**B. Backwards-compatible v1 + v2 dual-schema.**  Keep the v1 `os:` field
working alongside the new `cores:` block; pick whichever the file
declares.  Rejected because:

- No shipping customers — backwards compatibility costs maintenance for
  zero external constituents.
- Two parsers + two code paths + two schemas in CI doubles the surface
  area without earning anything.
- Examples doubly: the in-repo `board.yaml` files have to pick one
  schema, so every example would either teach the legacy form (wrong) or
  the new form (then the legacy form is dead code).

**C. Stay at mproc primitives.**  Don't ship `<alp/rpc.h>` or the
generated `<alp/system_ipc.h>`.  Customers compose mailboxes + shmem +
hwsem by hand.  Rejected because:

- Every example would re-implement framing + lifecycle by hand.  A
  meaningful chunk of the SDK's value proposition is the abstraction
  layer above the primitives; abdicating it for the cross-core case
  contradicts [ADR 0001](0001-wrapper-on-top-of-zephyr.md)'s "wrap
  Zephyr because portability lives above the primitives" framing.
- The endpoint-ID / carve-out-address allocation is exactly the kind of
  cross-vendor detail the SDK absorbs.  Pushing it to apps puts every
  customer on the hook for solving the same problem.

## References

- `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`
  — the full design spec this ADR ratifies.
- [`docs/heterogeneous-builds.md`](../heterogeneous-builds.md) —
  app-developer-facing walkthrough for writing a dual-app project.
- [`include/alp/mproc.h`](../../include/alp/mproc.h) — the low-level IPC
  primitives `<alp/rpc.h>` sits on top of.
- [ADR 0001](0001-wrapper-on-top-of-zephyr.md) — the layering philosophy
  that puts portable surfaces above vendor + OS primitives.
- [ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md) — the studio shares
  this same per-core mapping; the metadata stays in alp-sdk as
  single-source-of-truth.
- [OpenAMP project](https://www.openampproject.org/) — upstream
  reference for the RPMsg + virtio framing.
- Zephyr [`subsys/ipc/rpmsg_service`](https://docs.zephyrproject.org/latest/services/ipc/rpmsg_service/index.html)
  documentation.
- Linux [remoteproc + RPMsg subsystem](https://docs.kernel.org/staging/remoteproc.html)
  documentation.
