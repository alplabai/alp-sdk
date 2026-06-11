# Secure firmware-update log — design (slice 1: unified surface + software tier)

Status: Approved (brainstorming) — 2026-06-11
Author: alpCaner
Scope: first slice. The portable surface and its software tier ship now;
hardware enforcement (TF-M / OPTIGA) is a defined, stubbed seam for a
later slice.

## Context

A customer evaluating the E1M-AEN (Alif Ensemble E4) family asked for a
tamper-resistant, append-only firmware-update log: every update records
`{firmware version, image hash, update result/status, timestamp}` in
persistent memory, entries append, and **once written an entry must not
be modifiable or deletable by the application firmware**, even across
future updates. They asked whether the E4 (Secure Enclave / TF-M / secure
storage / monotonic counter / OTP) can back this.

The capability is feasible on the platform, but an audit on 2026-06-11
established that **none of the building blocks are implemented or proven**:
no audit-log/measured-boot/monotonic-counter code exists; TF-M on Alif is
a design decision (ADR 0013) only (no `sysbuild/tfm/`, no Alif board file,
never compiled); the OPTIGA driver and Alif SecAES storage are stubs that
return `NOSUPPORT`; `<alp/security.h>` provides only hash/AEAD/random with
the HW path wired for V2N alone; and no Alif silicon has been brought up
in this project.

## Design intent — this is a unification layer, not a one-off

alp-sdk's reason to exist is to be the **unification and simplification
layer**: the customer learns *one* portable `<alp/*>` API and the SDK
hides the vendor mechanism (Alif SE Services vs TF-M PSA vs OPTIGA host
library vs Renesas vs NXP). A secure update log must therefore ship as a
**portable surface with a working software tier today**, transparently
upgraded to hardware enforcement per-SoM — not as an internal example the
customer has to wire to TF-M and OPTIGA themselves.

The honesty requirement (never advertise more than is proven) is met not
by hiding the feature but by making the **assurance level queryable**: the
API tells the caller whether, on this silicon, the log is software
tamper-evident or hardware-enforced. Unified, simple, and honest at the
same time.

## Goals (this slice)

- A **portable surface** `<alp/update_log.h>`: `open / append / verify /
  iterate / assurance`. Vendor names never appear in application code.
- A **first-class software tier** (not a test fixture): a portable,
  tamper-evident hash-chain log that works on every target today,
  following the SDK rule "SW fallback preferred over `NOSUPPORT` when
  reasonable."
- A **queryable assurance level** so the caller knows what this SoM gives.
- A **defined hardware seam** (TF-M Protected Storage + monotonic counter,
  optional OPTIGA-rooted signing) present as a documented stub for a later
  slice — the "we're on it" contract.
- A **reference example** showing the firmware-update-log policy, runnable
  on `native_sim`.
- A **host ztest suite** + a **fuzz target** on the entry decode path.
- Written **acceptance criteria** defining `HW_ENFORCED` (the definition
  of "hardware-proven") for the later Alif bring-up.

## Non-goals (this slice — explicit)

- The public surface ships **experimental** (per `docs/abi-markers.md`) —
  no frozen ABI until it is hardware-proven.
- Do **not** build TF-M, an Alif board file, or real PSA Protected
  Storage; do not implement the real OPTIGA transport or a real hardware
  monotonic counter. The Alif backend is a present-but-unregistered stub
  seam; the software tier serves every SoM in this slice.
- Do **not** implement per-entry / per-head cryptographic signing yet.
- Use the backend-registry dispatch pattern with a universal
  `silicon_ref="*"` software tier, exactly like `<alp/security.h>` — that
  is the intended unification mechanism. But do **not** add an
  `update_log` entry to the **silicon caps matrix / `gen_soc_caps`**,
  change the `security.psa:` schema, or emit Kconfig claiming platform
  support — those assert hardware the SoM doesn't have yet.
- Honesty is carried by the **experimental marker + the assurance level**,
  never by over-claiming.

## Architecture

Three layers, cleanly separated:

```
Application
   │  <alp/update_log.h>           ← portable surface (the unification)
   ▼
src/update_log_dispatch.c          ← picks the backend, exposes assurance
   │  update_log_ops vtable
   ▼
backends:                          ← differ only in store + counter source
   sw_tier.c        (REAL, silicon_ref="*", fallback)   → SW_TAMPER_EVIDENT
   alif_tfm_stub.c  (seam only, not registered this slice) → HW_ENFORCED
   │  inject secure_store_if + monotonic_counter_if
   ▼
update-log engine (pure, dependency-free)
   entry encode/decode · hash-chain · append · verify
```

The **engine** holds the tamper-evidence logic and has no Zephyr/PSA
dependency, so the host tests exercise the real mechanism. Backends differ
only in which store + counter they inject; that is the single axis between
"software tier" and "hardware-enforced." This keeps the surface identical
across SoMs while the assurance level reflects the active backend.

In this slice only `sw_tier.c` registers, so every target resolves to the
software tier and reports `SW_TAMPER_EVIDENT`. When Alif bring-up lands, a
real `alif_tfm.c` registers at higher priority on Alif and the *same
application code* reports `HW_ENFORCED`.

## Module layout

```
include/alp/update_log.h                 -- portable surface (experimental)
src/update_log_dispatch.c                -- surface impl + backend selection
src/backends/update_log/
  update_log_ops.h                       -- backend vtable
  sw_tier.c                              -- REAL software tier (universal)
  alif_tfm_stub.c                        -- HW seam: PSA-PS + NV counter (stub)
src/update_log/
  engine.h, engine.c                     -- pure hash-chain core
  store.h                                -- secure_store_if + monotonic_counter_if
  store_sw.c                             -- in-process/file store + counter (sw_tier dep)
examples/connectivity/firmware-update-log/
  src/main.c, prj.conf, board.yaml, native_sim.conf, CMakeLists.txt, README.md
tests/unit/update_log/                   -- ztest suite, native_sim via twister
tests/fuzz/update_log_entry_fuzz.c
```

## Assurance levels (the honesty mechanism)

```c
typedef enum {
    ALP_UPDATE_LOG_SW_TAMPER_EVIDENT = 0, /* hash-chain + counter; app-cooperative.
                                             Detects mutation/truncation/rollback/
                                             reorder. Does NOT physically isolate
                                             the store from the application. */
    ALP_UPDATE_LOG_HW_ENFORCED       = 1, /* store in TF-M Protected Storage behind
                                             TrustZone; non-decrementable HW monotonic
                                             counter. Application physically cannot
                                             reach past entries. */
} alp_update_log_assurance_t;

alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log);
```

The customer queries this once and logs/branches on it. Same code on every
SoM; the value tells the truth about this silicon.

## Entry format

Canonical, deterministic encoding (fixed-endian, length-prefixed strings)
so hashing and parsing are reproducible across builds and platforms.

| Field        | Type            | Notes                                                        |
|--------------|-----------------|--------------------------------------------------------------|
| `seq`        | u64             | Authoritative ordering; monotonic; genesis = 0               |
| `fw_version` | length-prefixed | e.g. SemVer string                                           |
| `image_hash` | 32 bytes        | SHA-256 of the image MCUboot/SE verified                     |
| `status`     | u8 enum         | confirmed / verify-failed / rolled-back / pending-confirm    |
| `timestamp`  | u64 epoch       | **best-effort**; flagged "unset" when no trusted RTC         |
| `prev_hash`  | 32 bytes        | SHA-256 of previous entry's canonical bytes; zero at genesis |

`timestamp` is best-effort on purpose — an embedded target may lack a
trusted RTC when the entry is written. `seq` (bound to the monotonic
counter) is the authoritative ordering. Hashing is SHA-256 via a small
internal hash seam so the host build stays dependency-free; on a real
target the same logical hash is available via PSA.

## Tamper-evidence model — what each assurance level delivers

**Both tiers (proven now in host tests):**

- *Mutation of a past entry* → next entry's `prev_hash` mismatches →
  `verify()` reports chain-broken-at-seq-N.
- *Truncation of the tail* → stored count < monotonic counter → detected.
- *Rollback to an older snapshot* → counter regressed vs anchor → rejected.
- *Reorder* → `seq` / chain mismatch → detected.

**Only `HW_ENFORCED` adds (HW-gated, later slice):**

- *The store is physically unreachable by the non-secure application* —
  TF-M Protected Storage behind TrustZone. The software tier can present
  an append-only API but cannot *enforce* unreachability; this is the one
  property that requires hardware and is the gap between the two levels.
- *The counter is non-decrementable in hardware* (PSA NV counter / OPTIGA).
- *Optional per-head signature* by a device key that never leaves the
  secure element, for off-device verification.

So the software tier proves the **algorithm**; the hardware tier adds
**enforcement**. The assurance level names exactly which one the caller
has.

## The engine seams

```c
typedef struct {
    alp_status_t (*put)  (void *ctx, const char *key, const uint8_t *buf, size_t len);
    alp_status_t (*get)  (void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len);
    alp_status_t (*erase)(void *ctx, const char *key);
    void *ctx;
} alp_secure_store_if;

typedef struct {
    alp_status_t (*read)     (void *ctx, uint32_t id, uint64_t *out_val);
    alp_status_t (*increment)(void *ctx, uint32_t id, uint64_t *out_val);
    void *ctx;
} alp_monotonic_counter_if;
```

The engine touches no hardware — only these. `store_sw.c` implements both
for real (software tier); the future `alif_tfm.c` implements them over
PSA Protected Storage + a PSA NV / OPTIGA counter (`HW_ENFORCED`).

## Error handling

- All ops return `alp_status_t`. `verify()` returns a structured verdict:
  `OK`, `CHAIN_BROKEN` (with the offending `seq`), `TRUNCATED`,
  `ROLLED_BACK`. Detail via `alp_last_error()` per ADR 0002.
- No silent fallback: a store/counter op that fails propagates; `verify()`
  never reports `OK` on an unreadable or short-read store.

## Testing

- **Host ztest suite** under `tests/unit/update_log/`, run on `native_sim`
  via twister (the load-bearing local gate): append → verify happy path;
  each tamper class injected and asserted detected; counter-rollback
  rejected; genesis; capacity/wrap; corrupt-length decode returns an error
  (never `OK`); `assurance()` returns `SW_TAMPER_EVIDENT`.
- **Fuzz target** `tests/fuzz/update_log_entry_fuzz.c` on the entry decode
  path — the code that will later ingest attacker-reachable bytes from
  persistent flash — following the repo's `tests/fuzz/` convention.

## Acceptance criteria for `HW_ENFORCED` (definition of "hardware-proven")

When Alif/TF-M bring-up happens, `alif_tfm.c` replaces the stub and is
considered proven only when, on real silicon:

1. The store is a TF-M **Protected Storage** asset in the Secure
   Processing Environment; a deliberate non-secure read/write of the
   region faults (TrustZone/MPC enforced) — a negative test.
2. The counter is a hardware monotonic counter (PSA NV / OPTIGA) that
   cannot be decremented; a power-cycle + rollback attempt is rejected.
3. The same host test suite passes against the hardware backend on silicon.
4. (If signing is in scope) the log head is signed by a device key that
   never leaves the secure element, and an off-device verifier validates
   the chain.

Until all hold, the surface stays experimental and any SoM without the
backend reports `SW_TAMPER_EVIDENT` — never `HW_ENFORCED`.

## Graduation path (later slices, not now)

- Bring up the Alif Zephyr board file + a TF-M sysbuild child image.
- Implement `alif_tfm.c` (PSA Protected Storage + NV counter) and register
  it; assurance flips to `HW_ENFORCED` on Alif with no app-code change.
- Vendor the Infineon OPTIGA host library as a Zephyr module for the
  counter / optional signing.
- Fit V2N (OP-TEE / secure storage) and i.MX93 backends to the same seam.
- Once hardware-proven against the acceptance criteria, drop the
  experimental marker and freeze the ABI.

## Customer-facing implication

What we can honestly tell the customer today: the architecture is sound
and feasible on the E4; alp-sdk exposes **one portable API** for the update
log that works now via a software tamper-evident tier and **transparently
upgrades to TF-M/OPTIGA hardware enforcement** as the Alif backend lands —
the application code never changes, only the queryable assurance level
does. Hardware enforcement is not yet proven (pending the Alif bring-up).
In the meantime the same hardware guarantees are reachable directly
through Alif Semiconductor's own Secure Enclave / TF-M tooling, which the
customer can use immediately.
