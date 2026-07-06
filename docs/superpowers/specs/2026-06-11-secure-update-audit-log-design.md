# Secure firmware-update log — design (unified surface + hardware seam)

Status: Approved (brainstorming) — 2026-06-11; hardware-seam update — 2026-07-06
Author: alpCaner
Scope: portable surface, software tier, and TF-M secure-service hardware seam.
The hardware owner source now exists; per-SoM hardware assurance is still gated
on secure-image integration and silicon validation.

## Context

A customer evaluating the E1M-AEN (Alif Ensemble E4) family asked for a
tamper-resistant, append-only firmware-update log: every update records
`{firmware version, image hash, update result/status, timestamp}` in
persistent memory, entries append, and **once written an entry must not
be modifiable or deletable by the application firmware**, even across
future updates. They asked whether the E4 (Secure Enclave / TF-M / secure
storage / monotonic counter / OTP) can back this.

The capability is feasible on the platform. The 2026-06-11 audit established
that no audit-log/measured-boot/monotonic-counter path was implemented or
proven yet. The 2026-07-06 update adds the TF-M secure-service client and a PSA
Protected Storage owner source, but Alif E4/E8 assurance still needs the secure
M55 image, SE/firewall-protected storage placement, and on-silicon validation.

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

## Goals

- A **portable surface** `<alp/update_log.h>`: `open / append / verify /
  iterate / assurance`. Vendor names never appear in application code.
- A **first-class software tier** (not a test fixture): a portable,
  tamper-evident hash-chain log that works on every target today,
  following the SDK rule "SW fallback preferred over `NOSUPPORT` when
  reasonable."
- A **queryable assurance level** so the caller knows what this SoM gives.
- A **defined hardware seam**: non-secure client -> secure owner -> PSA
  Protected Storage + protected high-watermark counter, with optional
  OPTIGA-rooted signing later.
- A **reference example** showing the firmware-update-log policy, runnable
  on `native_sim`.
- A **host ztest suite** + a **fuzz target** on the entry decode path.
- Written **acceptance criteria** defining `HW_ENFORCED` (the definition
  of "hardware-proven") for the later Alif bring-up.

## Non-goals (explicit)

- The public surface ships **experimental** (per `docs/abi-markers.md`) —
  no frozen ABI until it is hardware-proven.
- Do **not** claim E4/E8 hardware assurance until the secure owner is wired
  into a secure M55/TF-M image and the storage region is proven behind the Alif
  SE/firewall policy. The non-secure app must not directly own the PSA UIDs.
- Do **not** implement the real OPTIGA transport yet.
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
   tfm_psa.c        (TF-M secure-service client)         → HW_ENFORCED
   │  calls secure owner seam
   ▼
secure owner:
   tfm_psa_secure_owner.c  (PSA Protected Storage + counter asset)
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

`sw_tier.c` registers everywhere, so every target has the software
tamper-evident path. `tfm_psa.c` registers at higher priority only when the
build enables the TF-M client, and its `ready()` probe binds only when a secure
owner answers. The same application code then reports `HW_ENFORCED`. On Alif
E4/E8, that owner must run in secure world and its storage must sit behind the
SE/firewall policy before the claim is meaningful on silicon.

## Module layout

```
include/alp/update_log.h                 -- portable surface (experimental)
src/update_log_dispatch.c                -- surface impl + backend selection
src/backends/update_log/
  update_log_ops.h                       -- backend vtable
  sw_tier.c                              -- REAL software tier (universal)
  tfm_psa.c                              -- HW tier client: secure-service seam
src/update_log/
  engine.h, engine.c                     -- pure hash-chain core
  secure_service.h                       -- internal secure-owner calls
  store.h                                -- secure_store_if + monotonic_counter_if
  tfm_psa_secure_owner.c                 -- secure owner: PSA Protected Storage
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
    ALP_UPDATE_LOG_HW_ENFORCED       = 1, /* secure owner stores entries in
                                             TF-M Protected Storage behind
                                             TrustZone; protected high-watermark
                                             counter. Application physically
                                             cannot reach past entries. */
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

**Only `HW_ENFORCED` adds (secure-backend gated):**

- *The store is physically unreachable by the non-secure application* —
  TF-M Protected Storage behind TrustZone. The software tier can present
  an append-only API but cannot *enforce* unreachability; this is the one
  property that requires hardware and is the gap between the two levels.
- *The high-watermark counter is protected against non-secure writes and
  replay* by the secure storage backend. TF-M Protected Storage depends on
  the platform NV-counter service for this replay-protection anchor.
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

The engine touches no hardware — only these. `sw_tier.c` implements both for
the software tier; `tfm_psa_secure_owner.c` implements them over PSA Protected
Storage for the hardware-enforced tier. `tfm_psa.c` is the non-secure client
that calls the secure owner.

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

`tfm_psa.c` plus `tfm_psa_secure_owner.c` are the implementation. It is
considered proven for a given SoM only when, on real silicon:

1. The store is a TF-M **Protected Storage** asset in the Secure
   Processing Environment; a deliberate non-secure read/write of the
   region faults or is denied (TrustZone/MPC/firewall enforced) — a negative test.
2. The Protected Storage replay-protection anchor rejects a power-cycle +
   rollback attempt instead of accepting the older store image.
3. The same host test suite passes against the hardware backend on silicon.
4. (If signing is in scope) the log head is signed by a device key that
   never leaves the secure element, and an off-device verifier validates
   the chain.

Until all hold, the surface stays experimental and any SoM without the
backend reports `SW_TAMPER_EVIDENT` — never `HW_ENFORCED`.

## Graduation path

- Bring up the Alif Zephyr board file + a TF-M sysbuild child image.
- Place TF-M Protected Storage in an Alif SE/firewall-protected MRAM window.
- Enable `CONFIG_ALP_SDK_UPDATE_LOG_TFM=y` in the non-secure app and wire the
  secure owner into the SPE; assurance flips to `HW_ENFORCED` with no app-code
  change.
- Vendor the Infineon OPTIGA host library as a Zephyr module for the
  counter / optional signing.
- Fit V2N (OP-TEE / secure storage) and i.MX93 backends to the same seam.
- Once hardware-proven against the acceptance criteria, drop the
  experimental marker and freeze the ABI.

## Customer-facing implication

What we can honestly tell the customer today: the architecture is sound and
feasible on the E4, and alp-sdk exposes **one portable API** for the update
log. It works everywhere through the software tamper-evident tier, and the
hardware-enforced TF-M secure-owner backend now exists behind the same API. E4
hardware assurance still needs the secure build and on-silicon validation that
the storage region is protected by the Alif SE/firewall policy; the application
code never changes, only the queryable assurance level does.
