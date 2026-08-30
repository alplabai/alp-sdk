# 0027. Storage regions are declared by ROLE; `flash_device:` becomes an explicit pin

Status: Proposed
Date: 2026-08-08 (Caner)
Deciders: alpCaner (alp-sdk)
Relates to: [0011](0011-intra-family-portability.md) (portability is
INTRA-family), [0020](0020-sdk-owns-build-execution.md) (the planner emits the
partition table)

## Context

The customer-facing question this answers: **"if I want a storage region, how
do I declare it — and what happens when I change the SoM?"**

Today `board.yaml`'s `storage:` entries place themselves with `flash_device:`,
a string naming a region in the SoM preset's `memory_map:` or
`on_module.ospi_memories:`. That couples every partition to one SoM's internal
naming.

How badly, measured across all 11 presets in `metadata/e1m_modules/` at
`f30f4d4b`:

| Preset | `memory_map:` | `on_module.ospi_memories:` |
|---|---|---|
| E1M-AEN801 | `mcuboot`, `he_slot0`, `hp_slot0`, `reserved`, `storage`, `atoc`, `mram_main` | yes |
| E1M-AEN301/401/501/601/701 | **absent** | yes |
| E1M-V2N101/102, E1M-V2M101/102, E1M-NX9101 | **absent** | **absent** (they carry `on_module.nor_flash` / `emmc` instead) |

So `flash_device:` is resolvable on **exactly one of eleven** SoMs. It is not
that the names differ between modules — on ten of them there are no named
regions to reference at all.

The one example that uses the field, `examples/connectivity/production-deployment/board.yaml`,
pins all five of its partitions to `mram_main` and declares `som.sku:
E1M-AEN801`. When this ADR was written that was portable to nothing. Since
#1447 all six AEN presets declare a `memory_map:` carrying `mram_main`, so an
intra-family retarget at E1M-AEN601 now resolves; the gap remains on the five
non-AEN presets (E1M-V2N101/102, E1M-V2M101/102, E1M-NX9101), which declare no
`memory_map:` at all.

That is the whole problem. A customer's answer to "how do I declare a region"
is currently "name a SoM-internal region", which is exactly the kind of
duplicated hardware fact the SDK exists to remove.

## Decision

**Intent is the primary surface; placement is the escape hatch.**

### 1. `role:` — the portable way, and the documented default

```yaml
storage:
  - { name: settings, role: settings,  fs: littlefs, size_kib: 64,  mount: /lfs/settings }
  - { name: app_data, role: app_data,  fs: littlefs, size_kib: 256, mount: /lfs/app }
```

`role:` states what the partition is *for*. The planner resolves it to a
concrete region **per SoM**, from the preset. Initial roles:

| Role | Meaning |
|---|---|
| `settings` | small, frequently-rewritten config that must survive an OTA |
| `app_data` | bulk application data |
| `log` | append-mostly diagnostics, first to be sacrificed when space is short |
| `ota_cache` | scratch/staging for an update, may be erased at any time |

That list is deliberately short. Roles are a **closed enum** validated by the
schema, not free text: an unknown role is a refusal, not a silently-ignored
field, so a typo cannot degrade into an unplaced partition.

### 2. `flash_device:` survives, with a narrower job

It stops being the ordinary way to place a partition and becomes an
**explicit pin** for the cases roles cannot express — a bring-up experiment, a
second OSPI part, a layout a customer must byte-match against something
external. Keeping it is deliberate: removing it would make the simple case
easy and the unusual case impossible, which is the opposite of "flexible yet
simple".

Rules:
- `role:` and `flash_device:` may both appear. `flash_device:` wins on
  placement — an explicit pin outranks inferred intent — and `role:` still
  carries the semantic (erase policy, OTA-survival, docs).
- A pinned entry is **not portable, and says so**: the planner emits a
  portability note naming the SoM it is pinned to.

### 3. What a SoM swap does — the question that motivated this

- **Role-only entries re-resolve.** Swap E1M-AEN801 → E1M-AEN601 and
  `role: settings` lands wherever that preset puts settings. No `board.yaml`
  edit.
- **Pinned entries refuse, by name.** If `flash_device: mram_main` does not
  exist on the new SoM, the planner **refuses** with the region that is
  missing, the SoM that lacks it, and the role that would replace it. It does
  not silently relocate the partition, and it does not fall back to offset 0 —
  either would move a customer's persisted data without telling them.

**A refusal here is the feature.** Relocating persisted settings because a
region name stopped resolving is data loss with a green build.

### 4. Sizes stay explicit

`size_kib` remains required. A role implies *where*, never *how much* — a
default size would silently differ per SoM and make an OTA image's layout
depend on a table the customer never saw.

## Consequences

**Good.** The simple case gets simpler: `role: settings` + `size_kib` is the
whole declaration, and it is the same text on every SoM. A SoM swap stops
being a `board.yaml` edit for the common case, which is what ADR 0011's
portability promise implies but storage did not deliver. The remaining
SoM-specific knowledge moves into the preset, where the SoM vendor (us) owns
it, instead of into the customer's `board.yaml`.

**Bad / accepted.** Roles are an abstraction to learn, and a customer who
wants byte-exact control must know to reach for `flash_device:`. The preset
must now carry a role→region mapping for **every** SoM, including the five with
no `memory_map:` today — that is real work, and until a given SoM has it,
`role:` cannot resolve there. Sequencing is in the migration below so the
feature never half-exists.

**Risk.** The refusal in clause 3 turns a previously-silent situation into a
hard failure. That is intended, but it means a customer upgrading the SDK with
an existing pinned `board.yaml` on a SoM they have already swapped will now
see an error where they saw a (wrong) build. The error text must name the fix,
not just the fault.

## Migration

1. **Add the role→region mapping to the presets, starting with the SoMs that
   have regions.** All six AEN presets (E1M-AEN301/401/501/601/701/801) already
   have a `memory_map:`; the V2N/V2M/NX parts with `nor_flash`/`emmc` each need
   theirs written from module truth, not inferred. **Where the backing part is
   not yet pinned down, the role is marked TBD and `role:` refuses on that SoM
   with "not yet mapped" — never a guessed region.**
2. Add `role:` to `metadata/schemas/board.schema.json` as a closed enum,
   accepted alongside `flash_device:`.
3. Teach the planner's allocator to resolve `role:` → region, keeping the
   existing reserved-span bounds check (#1331) and the AEN `atoc` reservation
   (#1289) — a role must never resolve onto a reserved band.
4. Convert `examples/connectivity/production-deployment/board.yaml`, the only
   current `flash_device:` user, to roles for its `settings` and `app_data`
   entries. Its three MCUboot slots stay pinned: they are byte-matched against
   the bootloader and are exactly the case the pin exists for.
5. Port the same change to `python/tan/planner/partition.py` — or, if
   [0026](0026-tan-owns-the-planner-outright.md) is accepted first, implement
   it once in tan and skip the port. **This ADR is a concrete instance of the
   duplication 0026 describes: without 0026, clause 3 is two implementations
   of the same refusal.**

   **Accepted is not executed (noted 2026-08-30).** 0026 moved to `Accepted`
   with an amendment on 2026-08-30, so the condition in this clause now reads
   as met — but its migration has not run. Until 0026's replacement step 5
   deletes the plan producer, alp-sdk still owns the emitters and still
   allocates partitions, so **the port is still required**: skipping it now
   would leave clause 3's reserved-band refusal implemented on one side only,
   diverging a hardware refusal across two live producers. That is the exact
   class 0026 exists to close, and 0026's own amendment §G orders the split and
   the repoint *before* any deletion for the same reason. Re-read this clause
   when 0026's step 5 lands; not before.

## Alternatives considered

- **Keep `flash_device:` as the only mechanism and document the swap cost.**
  Rejected: it makes the customer maintain a SoM-internal fact, and the
  measurement above shows the field cannot even be written on ten of eleven
  SoMs, so "document it" would be documenting something mostly unusable.
- **Auto-place everything; drop explicit placement entirely.** Rejected: the
  MCUboot slots in the one real example must byte-match the bootloader, and an
  allocator that cannot be overridden makes that unexpressible.
- **Infer the role from the partition `name:`** (`name: settings` implies the
  settings role). Rejected: it makes a label load-bearing, so renaming a
  partition for readability would silently move it — the same class of
  invisible coupling this ADR removes.
