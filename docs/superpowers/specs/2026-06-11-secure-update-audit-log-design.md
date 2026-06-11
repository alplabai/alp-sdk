# Secure firmware-update audit log — design (slice 1: provable core)

Status: Approved (brainstorming) — 2026-06-11
Author: alpCaner
Scope: first slice only. Hardware anchoring is explicitly out of this slice.

## Context

A customer evaluating the E1M-AEN (Alif Ensemble E4) family asked for a
tamper-resistant, append-only firmware-update audit log: every update
records `{firmware version, image hash, update result/status, timestamp}`
in persistent memory, new entries append, and **once written an entry
must not be modifiable or deletable by the application firmware**, even
across future updates. They asked whether the E4 (Secure Enclave / TF-M /
secure storage / monotonic counter / OTP) can back this, and how.

The capability is feasible on the platform, but an audit of the SDK on
2026-06-11 established that **none of the building blocks are implemented
or proven**:

- No audit-log / measured-boot / monotonic-counter code exists anywhere.
- TF-M on Alif is a design decision (ADR 0013) only — no `sysbuild/tfm/`
  config, no Alif Zephyr board file (TBD in an external repo); never
  compiled.
- The OPTIGA Trust M driver (`chips/optiga_trust_m/`) is a stub: I²C
  presence probe only; every real op returns `NOSUPPORT`.
- Alif SecAES secure storage (`src/backends/ext/alif/storage.c`) is a
  stub returning `NOSUPPORT`.
- `<alp/security.h>` (PSA crypto) is real but only the V2N TRNG HW path
  is wired; it provides hash/AEAD/random only — no storage, counters, or
  attestation.
- No Alif silicon has been brought up in this project; all proven
  silicon work is V2N/Renesas + GD32.

So we cannot *prove* the full feature against Alif silicon today. This
slice builds the part we **can** make real and test now — the
tamper-evidence logic — behind a clean seam that the Alif/TF-M hardware
anchor slots into later. "We're on it" = real, tested code. "Waiting to
prove" = the hardware anchor only.

## Goals (this slice)

- A dependency-free **audit-log core**: entry format, hash-chain, append,
  and verify, with software-detectable tamper / truncation / rollback /
  reorder.
- A **software/host implementation** of the two backing seams (secure
  store + monotonic counter) that is real and drives the tests.
- A **stub secure implementation** of those seams (the future
  Alif/TF-M/OPTIGA path) returning `NOSUPPORT`.
- A **reference example** showing the firmware-update-log *policy* on top
  of the core, runnable on `native_sim`.
- A **host test suite** plus a **fuzz target** on the entry decode path.
- Written **acceptance criteria** defining what the hardware anchor must
  satisfy when Alif bring-up happens (the definition of "proven").

## Non-goals (this slice — explicit)

To avoid the "reads as supported but isn't" trap that this audit
surfaced, this slice deliberately does **not**:

- build TF-M, an Alif board file, or any real PSA Protected Storage;
- implement the real OPTIGA Trust M transport or a real monotonic
  counter;
- implement per-entry / per-head cryptographic signing;
- expose a public `<alp/*>` header (no frozen ABI surface);
- change the `security.psa:` schema or emit any Kconfig that claims
  platform support;
- register an `ALP_BACKEND_REGISTER` capability or touch the caps matrix
  / `gen_soc_caps` (registering a cap implies silicon support).

The stub secure backend returns `NOSUPPORT`. Nothing in this slice should
read, in code or docs, as a supported silicon capability.

## Approach

**Pure-library core + two injected interfaces, with provided
implementations.**

The security-relevant logic (hash-chain + monotonic-counter binding)
lives in a **core that has no Zephyr/PSA dependencies** and talks to two
narrow seams passed in as interface structs: a *secure store* and a
*monotonic counter*. Because that logic lives in the core — not behind a
backend boundary — the host tests exercise the actual tamper-evidence
mechanism, not a mock of it. The core is standalone-friendly (usable from
hand-written firmware, per the SDK's standalone-first principle) and does
not drag in the capability registry.

Rejected alternatives:

- *Single "append-only store" interface* (backend owns rollback
  protection): pushes the interesting logic behind the seam, so the host
  test can't prove it. Defeats "provable core".
- *Full `ALP_BACKEND_REGISTER` capability now*: idiomatic but implies
  platform support and drags in the caps matrix while everything is
  stubbed. We graduate to that pattern when the surface goes public.

## Module layout

```
src/audit_log/
  audit_log.h            -- internal core API (append / verify / iterate)
  audit_log.c            -- entry encode/decode, hash-chain, append, verify
  store_sw.c             -- REAL host store + in-process monotonic counter
  store_secure_stub.c    -- Alif/TF-M PSA-PS + NV-counter/OPTIGA seam; NOSUPPORT
examples/connectivity/firmware-update-audit-log/
  src/main.c             -- policy demo (append on update, verify on boot, print)
  prj.conf, board.yaml, native_sim.conf, CMakeLists.txt, README.md
tests/unit/audit_log/    -- ztest suite, run on native_sim via twister
tests/fuzz/audit_log_entry_fuzz.c
```

Headers are internal only (`src/audit_log/audit_log.h`), not under
`include/alp/`.

## Entry format

Canonical, deterministic encoding (fixed-endian, length-prefixed strings)
so hashing and parsing are reproducible across builds and platforms.

| Field         | Type            | Notes                                                        |
|---------------|-----------------|--------------------------------------------------------------|
| `seq`         | u64             | Authoritative ordering; monotonic; genesis = 0               |
| `fw_version`  | length-prefixed | e.g. SemVer string                                           |
| `image_hash`  | 32 bytes        | SHA-256 of the image MCUboot/SE verified                     |
| `status`      | u8 enum         | confirmed / verify-failed / rolled-back / pending-confirm    |
| `timestamp`   | u64 epoch       | **best-effort**; flagged "unset" when no trusted RTC         |
| `prev_hash`   | 32 bytes        | SHA-256 of previous entry's canonical bytes; zero at genesis |

`timestamp` is best-effort on purpose: an embedded target may lack a
trusted RTC at the time the entry is written. `seq` (bound to the
monotonic counter) is the authoritative ordering; `timestamp` is
advisory metadata.

Hashing uses SHA-256. In the host build the core uses a bundled/portable
SHA-256 (no PSA dependency); on a real target the same logical hash is
available via `<alp/security.h>` / PSA. The hash primitive is a small
internal seam so the host build stays dependency-free.

## Tamper-evidence model — provable now vs hardware-gated

This split is the honest core of the whole feature and is reproduced in
the example README and the customer-facing note.

**Provable now (host tests, software-backed store + counter):**

- *Mutation of a past entry* → the next entry's `prev_hash` no longer
  matches → `verify()` reports chain-broken-at-seq-N.
- *Truncation of the tail* → stored entry count < monotonic-counter value
  → detected.
- *Rollback to an older store snapshot* → counter regressed vs the
  persisted anchor → rejected.
- *Reorder* → `seq` / chain mismatch → detected.

**Hardware-gated (the "waiting to prove" part — stubbed this slice):**

- *The store being physically unreachable by the non-secure application.*
  Only TF-M Protected Storage behind the TrustZone-M boundary gives this
  hard guarantee. The software store can *simulate* an append-only API
  but cannot *enforce* unreachability — any host code can edit host
  memory. This is the single most important property and it is
  HW-gated.
- *The counter being non-decrementable in hardware* (PSA NV counter /
  OPTIGA monotonic counter). The software counter is resettable.
- *Optional per-head signature* with a device key held in OPTIGA / PSA,
  for off-device third-party verification.

The software backend therefore proves the **algorithm**; the hardware
backend (future) proves the **enforcement**.

## The two seams

```c
/* Opaque keyed blob store. Host impl: RAM/file. Secure impl: PSA PS. */
typedef struct {
    alp_status_t (*put)  (void *ctx, const char *key,
                          const uint8_t *buf, size_t len);
    alp_status_t (*get)  (void *ctx, const char *key,
                          uint8_t *buf, size_t cap, size_t *out_len);
    alp_status_t (*erase)(void *ctx, const char *key);
    void *ctx;
} alp_secure_store_if;

/* Monotonic counter. Host impl: in-process. Secure impl: PSA NV / OPTIGA. */
typedef struct {
    alp_status_t (*read)     (void *ctx, uint32_t id, uint64_t *out_val);
    alp_status_t (*increment)(void *ctx, uint32_t id, uint64_t *out_val);
    void *ctx;
} alp_monotonic_counter_if;
```

The core never touches hardware — only these. `store_sw.c` implements both
for real; `store_secure_stub.c` implements both as `NOSUPPORT` placeholders
that document the intended PSA-PS / NV-counter / OPTIGA mapping.

## Error handling

- All ops return `alp_status_t` (project convention); the stub secure
  backend returns `ALP_ERR_NOSUPPORT`.
- `audit_log_verify()` returns a structured verdict: `OK`,
  `CHAIN_BROKEN` (with the offending `seq`), `TRUNCATED`, `ROLLED_BACK`.
- Detail is available via `alp_last_error()` per ADR 0002.
- No silent fallback: a store/counter op that fails propagates; verify
  never reports `OK` on an unreadable or short-read store.

## Testing

**Host unit tests** — a `ztest` suite under `tests/unit/audit_log/` run
on `native_sim` via twister (the load-bearing local CI gate):

- append → verify happy path across N entries;
- each tamper class above is injected and asserted detected;
- counter-rollback rejected; genesis handling; capacity/wrap behaviour;
- short-read / corrupt-length decode returns an error, never `OK`.

**Fuzz target** `tests/fuzz/audit_log_entry_fuzz.c` on the entry
decode path — this is the code that will later ingest attacker-reachable
bytes from persistent flash, so it is hardened from the start, following
the repo's existing `tests/fuzz/` convention.

## Acceptance criteria for the hardware anchor (definition of "proven")

When Alif/TF-M bring-up happens, the secure backend replaces
`store_secure_stub.c` and is considered "proven" only when, on real
silicon:

1. The store is a TF-M **Protected Storage** asset in the Secure
   Processing Environment; a non-secure application read/write of the
   underlying region faults (TrustZone/MPC enforced), demonstrated by a
   deliberate negative test.
2. The counter is a hardware monotonic counter (PSA NV counter or OPTIGA)
   that cannot be decremented; a power-cycle + rollback attempt is
   rejected.
3. The same host test suite (append / verify / tamper / truncation /
   rollback / reorder) passes against the secure backend on hardware.
4. (If signing is in scope) the log head is signed by a device key that
   never leaves the secure element, and an off-device verifier validates
   the chain.

Until all four hold, the feature is described as "in progress, hardware
proof pending" — never as "supported".

## Graduation path (future slices, not now)

- Bring up the Alif Zephyr board file + a TF-M sysbuild child image.
- Implement `store_secure_psa.c` (PSA PS + NV counter) replacing the stub.
- Vendor the Infineon OPTIGA host library as a Zephyr module; back the
  counter / optional signing with it.
- Once silicon-proven against the acceptance criteria, promote the
  internal core to a public portable surface and document it.

## Customer-facing implication

What we can honestly tell the customer today: the architecture is sound
and feasible on the E4; we have started implementing the tamper-evidence
core and are building toward a hardware-anchored proof; the hardware
enforcement (TF-M Protected Storage + monotonic counter + optional
OPTIGA-rooted signing) is pending the Alif bring-up and is not yet
proven. In the meantime the same guarantees are available directly
through Alif Semiconductor's own Secure Enclave / TF-M tooling, which the
customer can use now.
