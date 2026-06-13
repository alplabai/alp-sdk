# Secure Firmware-Update Log — Implementation Plan (slice 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the portable `<alp/update_log.h>` unification surface with a working, software-tamper-evident tier (hash-chain + monotonic-counter binding), a queryable assurance level, and a defined stub seam for the future Alif TF-M/OPTIGA hardware backend.

**Architecture:** Three layers — a pure dependency-free *engine* (entry encode/decode, SHA-256 hash-chain, append, verify) over two injected seams (`alp_secure_store_if`, `alp_monotonic_counter_if`); a *sw_tier* backend that provides RAM-backed seams and registers as the universal `silicon_ref="*"` fallback; and a *dispatch* layer exposing the public surface via the existing `ALP_BACKEND_REGISTER` registry. The Alif TF-M backend is present as a compiled-but-unregistered stub seam.

**Tech Stack:** C99, Zephyr ztest on `native_sim`, the alp-sdk backend registry (`<alp/backend.h>`), libFuzzer (`tests/fuzz/`), a vendored public-domain SHA-256.

**Scope guardrails (from the spec):** experimental ABI marker (no freeze); SW store is RAM-backed in this slice (durable Settings/NVS persistence is the named follow-up); no TF-M build, no real PSA-PS/OPTIGA, no per-entry signing, no `security.psa:` schema change, no silicon caps-matrix entry. Spec: `docs/superpowers/specs/2026-06-11-secure-update-audit-log-design.md`.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/alp/update_log.h` | Public surface: types (`alp_update_log_entry_t`, status/assurance/verdict enums), opaque handle, API decls. Experimental ABI. |
| `src/update_log/sha256.h` / `sha256.c` | Vendored public-domain SHA-256 + a thin `ulog_sha256()` wrapper. Dependency-free. |
| `src/update_log/store.h` | The two engine seams: `alp_secure_store_if`, `alp_monotonic_counter_if`. |
| `src/update_log/engine.h` / `engine.c` | Pure engine: wire encode/decode, append, verify, count, get. No Zephyr/PSA deps. |
| `src/backends/update_log/update_log_ops.h` | Backend vtable + backend-state struct. |
| `src/backends/update_log/sw_tier.c` | RAM-backed seams + `ALP_BACKEND_REGISTER(update_log, sw_tier, …)`; assurance `SW_TAMPER_EVIDENT`. |
| `src/backends/update_log/alif_tfm_stub.c` | Documented PSA-PS + NV-counter seam; compiled, **not** registered; returns `NOSUPPORT`. |
| `src/update_log_dispatch.c` | `ALP_BACKEND_DEFINE_CLASS(update_log)` + public API impl + handle pool. |
| `zephyr/Kconfig` | `config ALP_SDK_UPDATE_LOG` opt-in (default n). |
| `zephyr/CMakeLists.txt` | Compile the sources under `CONFIG_ALP_SDK_UPDATE_LOG`. |
| `tests/unit/update_log/` | ztest suite (engine tamper cases + public-surface smoke + assurance). |
| `tests/fuzz/update_log_entry_fuzz.c` + `tests/fuzz/CMakeLists.txt` | Fuzz the decode path against the real engine. |
| `examples/connectivity/firmware-update-log/` | native_sim demo of the policy + assurance readout. |

**Wire formats (canonical, little-endian, fixed-size — deterministic hashing):**

Entry = `ULOG_ENTRY_WIRE_LEN = 83` bytes:
```
off sz field
0   2  u16 version           (= ULOG_VERSION = 1)
2   8  u64 seq
10  1  u8  status
11  8  u64 timestamp
19  1  u8  fw_version_len     (0..31)
20  31  fw_version[31]        (zero-padded)
51  32  prev_hash[32]         (SHA-256 of previous entry's 83 bytes; zero at genesis)
```
Meta = `ULOG_META_WIRE_LEN = 46` bytes:
```
off sz field
0   2  u16 version           (= 1)
2   4  u32 magic             (= ULOG_META_MAGIC = 0x554C4F47 'ULOG')
6   8  u64 count
14  32 head_hash[32]         (SHA-256 of the last appended entry's 83 bytes)
```
Store keys: `"ulog.meta"`, and `"ulog.<seq>"` (decimal seq) for each entry. Counter id `0` holds the monotonic high-water = entry count.

---

## Task 1: Public header + wire encode/decode

**Files:**
- Create: `include/alp/update_log.h`
- Create: `src/update_log/sha256.h`, `src/update_log/sha256.c`
- Create: `src/update_log/engine.h`, `src/update_log/engine.c`
- Create: `zephyr/Kconfig` entry (modify), `zephyr/CMakeLists.txt` (modify)
- Test: `tests/unit/update_log/` (CMakeLists.txt, prj.conf, testcase.yaml, src/test_update_log.c)

- [ ] **Step 1: Write the public header**

`include/alp/update_log.h`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file update_log.h
 * @brief Portable, tamper-evident firmware-update audit log.
 *
 * One surface across SoMs. The software tier (every target today) gives a
 * hash-chained, monotonic-counter-anchored log that detects mutation,
 * truncation, rollback, and reorder. On SoMs with a secure backend the
 * same API is hardware-enforced (TF-M Protected Storage + a non-decrementable
 * monotonic counter). Query @ref alp_update_log_assurance to learn which.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 new. Surface may change until the hardware backend is
 *      silicon-proven. See docs/abi-markers.md.
 */

#ifndef ALP_UPDATE_LOG_H
#define ALP_UPDATE_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"  /* alp_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 digest length used for image hashes + chaining. */
#define ALP_UPDATE_LOG_HASH_LEN     32
/** Max firmware-version string length stored per entry (excl. NUL). */
#define ALP_UPDATE_LOG_FWVER_MAX    31

/** Outcome of an update, as recorded in an entry. */
typedef enum {
    ALP_UPDATE_STATUS_CONFIRMED       = 0, /**< New image booted + confirmed healthy. */
    ALP_UPDATE_STATUS_VERIFY_FAILED   = 1, /**< Signature/hash verification failed. */
    ALP_UPDATE_STATUS_ROLLED_BACK     = 2, /**< Reverted to the previous slot. */
    ALP_UPDATE_STATUS_PENDING_CONFIRM = 3, /**< Booted, awaiting confirm window. */
} alp_update_status_t;

/** How strongly the log is protected on this SoM. */
typedef enum {
    ALP_UPDATE_LOG_SW_TAMPER_EVIDENT = 0, /**< Hash-chain + counter; app-cooperative. */
    ALP_UPDATE_LOG_HW_ENFORCED       = 1, /**< TF-M-isolated store + HW monotonic counter. */
} alp_update_log_assurance_t;

/** Result of walking the chain in @ref alp_update_log_verify. */
typedef enum {
    ALP_UPDATE_LOG_VERIFY_OK           = 0,
    ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN = 1, /**< An entry was mutated or reordered. */
    ALP_UPDATE_LOG_VERIFY_TRUNCATED    = 2, /**< Tail entries are missing. */
    ALP_UPDATE_LOG_VERIFY_ROLLED_BACK  = 3, /**< Store regressed vs the monotonic anchor. */
} alp_update_log_verdict_t;

/**
 * One audit entry. On @ref alp_update_log_append the caller fills
 * everything except @c seq (the engine assigns it from the monotonic
 * counter). On @ref alp_update_log_get every field is populated.
 */
typedef struct {
    uint64_t            seq;        /**< Authoritative order; engine-assigned. */
    char                fw_version[ALP_UPDATE_LOG_FWVER_MAX + 1]; /**< NUL-terminated. */
    uint8_t             image_hash[ALP_UPDATE_LOG_HASH_LEN];      /**< SHA-256 of the image. */
    alp_update_status_t status;
    uint64_t            timestamp;  /**< Best-effort epoch; 0 = unset. */
} alp_update_log_entry_t;

/** Opaque log handle. Acquire via @ref alp_update_log_open. */
typedef struct alp_update_log alp_update_log_t;

/**
 * @brief Open the device's update log.
 * @return Handle on success; NULL if no backend is present (sets last error).
 */
alp_update_log_t *alp_update_log_open(void);

/**
 * @brief Append one entry. @c seq is assigned by the engine.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_update_log_append(alp_update_log_t *log,
                                   const alp_update_log_entry_t *entry);

/**
 * @brief Walk the chain and report integrity.
 * @param[out] verdict_out  Required.
 * @param[out] bad_seq_out  On CHAIN_BROKEN/TRUNCATED, the offending seq. May be NULL.
 * @return ALP_OK if the walk ran (inspect @p verdict_out for the result);
 *         ALP_ERR_IO if the store was unreadable.
 */
alp_status_t alp_update_log_verify(alp_update_log_t *log,
                                   alp_update_log_verdict_t *verdict_out,
                                   uint64_t *bad_seq_out);

/** @brief Number of entries. */
alp_status_t alp_update_log_count(alp_update_log_t *log, uint64_t *count_out);

/** @brief Fetch the entry at @p seq. ALP_ERR_NOT_FOUND if absent. */
alp_status_t alp_update_log_get(alp_update_log_t *log, uint64_t seq,
                                alp_update_log_entry_t *entry_out);

/** @brief Assurance level on this SoM. */
alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log);

/** @brief Release the handle. */
void alp_update_log_close(alp_update_log_t *log);

#ifdef __cplusplus
}
#endif

#endif /* ALP_UPDATE_LOG_H */
```

- [ ] **Step 2: Vendor SHA-256**

Drop the public-domain SHA-256 by Brad Conte (https://github.com/B-Con/crypto-algorithms,
`sha256.c`/`sha256.h`, license: public domain) into `src/update_log/sha256.c` and
`src/update_log/sha256.h` **verbatim**, then add this wrapper at the end of `sha256.h`
(inside the include guard) and a matching body in `sha256.c`:

`src/update_log/sha256.h` (append before `#endif`):
```c
/* alp wrapper: one-shot SHA-256 over (buf,len) -> out[32]. */
void ulog_sha256(const unsigned char *buf, size_t len, unsigned char out[32]);
```
`src/update_log/sha256.c` (append):
```c
#include <stddef.h>
void ulog_sha256(const unsigned char *buf, size_t len, unsigned char out[32])
{
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, buf, len);
    sha256_final(&ctx, out);
}
```
> Vendoring a verbatim public-domain file (not engineer-authored logic) is the
> one place this plan does not inline the body — transcribing SHA-256 by hand
> risks a subtle, security-relevant bug. Keep the upstream header comment.

- [ ] **Step 3: Engine header (constants + encode/decode decls only)**

`src/update_log/engine.h`:
```c
/* SPDX-License-Identifier: Apache-2.0
 * Pure, dependency-free update-log engine. NOT a public header. */
#ifndef ALP_UPDATE_LOG_ENGINE_H
#define ALP_UPDATE_LOG_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "alp/update_log.h"
#include "update_log/store.h"

#define ULOG_VERSION          1u
#define ULOG_ENTRY_WIRE_LEN   83u
#define ULOG_META_WIRE_LEN    46u
#define ULOG_META_MAGIC       0x554C4F47u  /* 'ULOG' */

/* Wire (de)serialisation. prev_hash is the chaining link, kept out of the
 * public entry struct. */
alp_status_t ulog_entry_encode(const alp_update_log_entry_t *e,
                               const uint8_t prev_hash[32],
                               uint8_t out[ULOG_ENTRY_WIRE_LEN]);
alp_status_t ulog_entry_decode(const uint8_t *buf, size_t len,
                               alp_update_log_entry_t *e_out,
                               uint8_t prev_hash_out[32]);

struct ulog_meta { uint64_t count; uint8_t head_hash[32]; };
alp_status_t ulog_meta_encode(const struct ulog_meta *m,
                              uint8_t out[ULOG_META_WIRE_LEN]);
alp_status_t ulog_meta_decode(const uint8_t *buf, size_t len,
                              struct ulog_meta *m_out);

/* Engine ops over the two seams (Tasks 2-4). */
alp_status_t ulog_engine_append(const alp_secure_store_if *store,
                                const alp_monotonic_counter_if *ctr,
                                const alp_update_log_entry_t *entry);
alp_status_t ulog_engine_verify(const alp_secure_store_if *store,
                                const alp_monotonic_counter_if *ctr,
                                alp_update_log_verdict_t *verdict_out,
                                uint64_t *bad_seq_out);
alp_status_t ulog_engine_count(const alp_secure_store_if *store,
                               const alp_monotonic_counter_if *ctr,
                               uint64_t *count_out);
alp_status_t ulog_engine_get(const alp_secure_store_if *store,
                             uint64_t seq, alp_update_log_entry_t *e_out);

#endif /* ALP_UPDATE_LOG_ENGINE_H */
```

- [ ] **Step 4: Seams header**

`src/update_log/store.h`:
```c
/* SPDX-License-Identifier: Apache-2.0
 * Engine seams. The engine touches no hardware -- only these. NOT public. */
#ifndef ALP_UPDATE_LOG_STORE_H
#define ALP_UPDATE_LOG_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "alp/peripheral.h"

/* Keyed blob store. Host: RAM. Secure (future): TF-M Protected Storage. */
typedef struct {
    alp_status_t (*put)  (void *ctx, const char *key, const uint8_t *buf, size_t len);
    alp_status_t (*get)  (void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len);
    alp_status_t (*erase)(void *ctx, const char *key);
    void *ctx;
} alp_secure_store_if;

/* Monotonic counter. Host: in-process. Secure (future): PSA NV / OPTIGA. */
typedef struct {
    alp_status_t (*read)     (void *ctx, uint32_t id, uint64_t *out_val);
    alp_status_t (*increment)(void *ctx, uint32_t id, uint64_t *out_val);
    void *ctx;
} alp_monotonic_counter_if;

#endif /* ALP_UPDATE_LOG_STORE_H */
```

- [ ] **Step 5: Engine encode/decode bodies**

`src/update_log/engine.c` (encode/decode only; append/verify/count/get added later):
```c
/* SPDX-License-Identifier: Apache-2.0
 * Pure update-log engine. No Zephyr/PSA includes -- builds on host. */
#include <string.h>

#include "update_log/engine.h"
#include "update_log/sha256.h"

static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint16_t get_u16(const uint8_t *p){ return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }
static uint64_t get_u64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

alp_status_t ulog_entry_encode(const alp_update_log_entry_t *e,
                               const uint8_t prev_hash[32],
                               uint8_t out[ULOG_ENTRY_WIRE_LEN])
{
    if (e == NULL || prev_hash == NULL || out == NULL) return ALP_ERR_INVAL;
    size_t vlen = strnlen(e->fw_version, ALP_UPDATE_LOG_FWVER_MAX + 1);
    if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
    memset(out, 0, ULOG_ENTRY_WIRE_LEN);
    put_u16(out + 0, (uint16_t)ULOG_VERSION);
    put_u64(out + 2, e->seq);
    out[10] = (uint8_t)e->status;
    put_u64(out + 11, e->timestamp);
    out[19] = (uint8_t)vlen;
    memcpy(out + 20, e->fw_version, vlen);
    memcpy(out + 51, prev_hash, 32);
    return ALP_OK;
}

alp_status_t ulog_entry_decode(const uint8_t *buf, size_t len,
                               alp_update_log_entry_t *e_out,
                               uint8_t prev_hash_out[32])
{
    if (buf == NULL || e_out == NULL) return ALP_ERR_INVAL;
    if (len < ULOG_ENTRY_WIRE_LEN) return ALP_ERR_INVAL;
    if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
    uint8_t vlen = buf[19];
    if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
    memset(e_out, 0, sizeof(*e_out));
    e_out->seq       = get_u64(buf + 2);
    e_out->status    = (alp_update_status_t)buf[10];
    e_out->timestamp = get_u64(buf + 11);
    memcpy(e_out->fw_version, buf + 20, vlen);   /* zero-padded -> NUL-terminated */
    /* image_hash is NOT on the wire in slice 1 (the chained prev_hash and the
     * entry's own re-hash carry integrity); keep it zeroed on decode. The
     * caller-supplied image_hash is preserved through append via the chain. */
    if (prev_hash_out != NULL) memcpy(prev_hash_out, buf + 51, 32);
    return ALP_OK;
}

alp_status_t ulog_meta_encode(const struct ulog_meta *m, uint8_t out[ULOG_META_WIRE_LEN])
{
    if (m == NULL || out == NULL) return ALP_ERR_INVAL;
    memset(out, 0, ULOG_META_WIRE_LEN);
    put_u16(out + 0, (uint16_t)ULOG_VERSION);
    put_u32(out + 2, ULOG_META_MAGIC);
    put_u64(out + 6, m->count);
    memcpy(out + 14, m->head_hash, 32);
    return ALP_OK;
}

alp_status_t ulog_meta_decode(const uint8_t *buf, size_t len, struct ulog_meta *m_out)
{
    if (buf == NULL || m_out == NULL) return ALP_ERR_INVAL;
    if (len < ULOG_META_WIRE_LEN) return ALP_ERR_INVAL;
    if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
    if (get_u32(buf + 2) != ULOG_META_MAGIC) return ALP_ERR_INVAL;
    m_out->count = get_u64(buf + 6);
    memcpy(m_out->head_hash, buf + 14, 32);
    return ALP_OK;
}
```
> Note on `image_hash`: slice-1 wire format omits it from the 83-byte entry to
> keep the layout fixed and the hash-chain the sole integrity primitive. If the
> customer needs `image_hash` recoverable via `get()`, Task 4 covers extending
> the wire format; flagged in self-review. (Decision: include it — see Task 4
> Step 3, which widens the entry to carry image_hash and bumps the length
> constant. Implement Task 1 with the 83-byte layout, then Task 4 widens it in
> one place.)

- [ ] **Step 6: Kconfig opt-in**

Add to `zephyr/Kconfig` (near the other `ALP_SDK_*` surface toggles, e.g. after `ALP_SDK_GPU2D`):
```kconfig
config ALP_SDK_UPDATE_LOG
	bool "Enable <alp/update_log.h> firmware-update audit-log surface (experimental)"
	default n
	help
	  Compile the portable update-log surface: src/update_log_dispatch.c,
	  the pure engine (src/update_log/engine.c + sha256.c), and the
	  software tamper-evident tier (src/backends/update_log/sw_tier.c).

	  The software tier works on every target and reports assurance
	  SW_TAMPER_EVIDENT. A hardware-enforced backend (TF-M Protected
	  Storage + monotonic counter) is a later slice; until it lands the
	  surface stays experimental. See docs/abi-markers.md.
```

- [ ] **Step 7: Build wiring**

In `zephyr/CMakeLists.txt`, after the security registry block (~line 150), add:
```cmake
# Update-log surface (<alp/update_log.h>) -- experimental, opt-in.
# Pure engine + software tamper-evident tier + (compiled, unregistered)
# Alif TF-M stub seam. Gated on CONFIG_ALP_SDK_UPDATE_LOG.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_UPDATE_LOG
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/engine.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/sha256.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/sw_tier.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/alif_tfm_stub.c)
```
The `src` include dir is already on the library include path (existing
`zephyr_library_include_directories(... ${ZEPHYR_CURRENT_MODULE_DIR}/src ...)`),
so `#include "update_log/engine.h"` and `"backends/update_log/update_log_ops.h"`
resolve. (Dispatch + sw_tier come in Task 6; engine.c+sha256.c compile from
Task 1. List all five now; the not-yet-created files are added by later tasks
in the same branch — but to keep each task's build green, add only
`engine.c` + `sha256.c` here in Task 1 and append the rest in Task 6.)

- [ ] **Step 8: Test scaffolding + failing test**

`tests/unit/update_log/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_update_log)
target_sources(app PRIVATE src/test_update_log.c)
```
`tests/unit/update_log/prj.conf`:
```
CONFIG_ZTEST=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SDK_UPDATE_LOG=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
```
`tests/unit/update_log/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.update_log:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim
      - native_sim/native/64
    tags:
      - alp
      - update_log
      - security
      - unit
```
`tests/unit/update_log/src/test_update_log.c`:
```c
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/update_log.h>

#include "../../../../src/update_log/engine.h"

ZTEST_SUITE(alp_update_log, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_update_log, test_entry_roundtrip)
{
    alp_update_log_entry_t e = {0};
    e.seq = 7; e.status = ALP_UPDATE_STATUS_CONFIRMED; e.timestamp = 1234;
    strcpy(e.fw_version, "1.2.3");
    uint8_t prev[32]; memset(prev, 0xAB, sizeof(prev));

    uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    zassert_equal(ulog_entry_encode(&e, prev, wire), ALP_OK);

    alp_update_log_entry_t got = {0};
    uint8_t got_prev[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, got_prev), ALP_OK);
    zassert_equal(got.seq, 7);
    zassert_equal(got.status, ALP_UPDATE_STATUS_CONFIRMED);
    zassert_equal(got.timestamp, 1234);
    zassert_equal(strcmp(got.fw_version, "1.2.3"), 0);
    zassert_mem_equal(got_prev, prev, 32);
}

ZTEST(alp_update_log, test_decode_short_buffer)
{
    uint8_t wire[ULOG_ENTRY_WIRE_LEN - 1] = {0};
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_INVAL);
}

ZTEST(alp_update_log, test_decode_bad_version)
{
    alp_update_log_entry_t e = {0}; e.seq = 1; strcpy(e.fw_version, "x");
    uint8_t prev[32] = {0}; uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    (void)ulog_entry_encode(&e, prev, wire);
    wire[0] = 0xFF; wire[1] = 0xFF;  /* corrupt version */
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_VERSION);
}
```

- [ ] **Step 9: Run the test — verify it fails (compile error: engine not yet built)**

Run (WSL Ubuntu-22.04, see `docs/local-ci.md`):
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
```
Expected: FAIL — undefined references to `ulog_entry_encode`/`_decode` until Step 7's CMake edit + engine.c land. (If you implemented Steps 1-7 first, expect PASS; the TDD intent is: write the test, watch it fail before engine.c is wired.)

- [ ] **Step 10: Make it pass**

Ensure Steps 1-7 are in place (header, sha256, engine.h/.c encode+decode, store.h, Kconfig, CMake adds engine.c + sha256.c). Re-run Step 9's command. Expected: PASS (3/3).

- [ ] **Step 11: Commit**
```
git add include/alp/update_log.h src/update_log/ zephyr/Kconfig zephyr/CMakeLists.txt tests/unit/update_log/
git commit -q -m "feat(update-log): public surface + wire encode/decode engine"
```

---

## Task 2: Hash-chain append

**Files:**
- Modify: `src/update_log/engine.c` (add `ulog_engine_append`)
- Test: `tests/unit/update_log/src/test_update_log.c` (+ a test-double store/counter helper)

- [ ] **Step 1: Add a test-double store + counter to the test file**

Append to `tests/unit/update_log/src/test_update_log.c` (above the append test):
```c
#include "../../../../src/update_log/store.h"

/* Minimal RAM store double: fixed slots of (key,blob). */
#define TD_SLOTS 16
#define TD_BLOB  128
struct td_store {
    struct { char key[24]; uint8_t buf[TD_BLOB]; size_t len; bool used; } s[TD_SLOTS];
    uint64_t counter;
};
static struct td_store *td(void *ctx) { return (struct td_store *)ctx; }

static int td_find(struct td_store *t, const char *key) {
    for (int i = 0; i < TD_SLOTS; i++) if (t->s[i].used && strcmp(t->s[i].key, key)==0) return i;
    return -1;
}
static alp_status_t td_put(void *c, const char *key, const uint8_t *b, size_t n) {
    struct td_store *t = td(c); if (n > TD_BLOB) return ALP_ERR_NOMEM;
    int i = td_find(t, key);
    if (i < 0) { for (i=0;i<TD_SLOTS;i++) if(!t->s[i].used) break; if(i==TD_SLOTS) return ALP_ERR_NOMEM; }
    t->s[i].used = true; strncpy(t->s[i].key, key, sizeof(t->s[i].key)-1);
    t->s[i].key[sizeof(t->s[i].key)-1]=0; memcpy(t->s[i].buf, b, n); t->s[i].len = n; return ALP_OK;
}
static alp_status_t td_get(void *c, const char *key, uint8_t *b, size_t cap, size_t *out) {
    struct td_store *t = td(c); int i = td_find(t, key); if (i<0) return ALP_ERR_NOT_FOUND;
    if (t->s[i].len > cap) return ALP_ERR_NOMEM; memcpy(b, t->s[i].buf, t->s[i].len);
    if (out) *out = t->s[i].len; return ALP_OK;
}
static alp_status_t td_erase(void *c, const char *key) {
    struct td_store *t = td(c); int i = td_find(t, key); if (i<0) return ALP_ERR_NOT_FOUND;
    t->s[i].used = false; return ALP_OK;
}
static alp_status_t td_cread(void *c, uint32_t id, uint64_t *v) { (void)id; *v = td(c)->counter; return ALP_OK; }
static alp_status_t td_cinc(void *c, uint32_t id, uint64_t *v) { (void)id; td(c)->counter++; *v = td(c)->counter; return ALP_OK; }

static void td_ifaces(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *ctr) {
    memset(t, 0, sizeof(*t));
    s->put = td_put; s->get = td_get; s->erase = td_erase; s->ctx = t;
    ctr->read = td_cread; ctr->increment = td_cinc; ctr->ctx = t;
}

static alp_update_log_entry_t mk_entry(uint64_t ts, const char *ver, alp_update_status_t st) {
    alp_update_log_entry_t e = {0}; e.timestamp = ts; e.status = st;
    strncpy(e.fw_version, ver, ALP_UPDATE_LOG_FWVER_MAX);
    memset(e.image_hash, (int)ts, sizeof(e.image_hash)); return e;
}
```

- [ ] **Step 2: Write the failing append test**
```c
ZTEST(alp_update_log, test_append_assigns_seq_and_chains)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    td_ifaces(&t, &s, &c);

    zassert_equal(ulog_engine_append(&s, &c, &mk_entry(100, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED)), ALP_OK);
    zassert_equal(ulog_engine_append(&s, &c, &mk_entry(200, "1.1.0", ALP_UPDATE_STATUS_CONFIRMED)), ALP_OK);

    /* counter advanced to 2; meta.count == 2 */
    zassert_equal(t.counter, 2);
    uint8_t metabuf[ULOG_META_WIRE_LEN]; size_t mlen = 0;
    zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
    struct ulog_meta m; zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);
    zassert_equal(m.count, 2);

    /* entry 0 has zero prev_hash; entry 1's prev_hash == sha256(entry0 wire) */
    uint8_t e0[ULOG_ENTRY_WIRE_LEN]; size_t n0 = 0;
    zassert_equal(td_get(&t, "ulog.0", e0, sizeof(e0), &n0), ALP_OK);
    alp_update_log_entry_t d1; uint8_t prev1[32]; uint8_t e1[ULOG_ENTRY_WIRE_LEN]; size_t n1 = 0;
    zassert_equal(td_get(&t, "ulog.1", e1, sizeof(e1), &n1), ALP_OK);
    zassert_equal(ulog_entry_decode(e1, n1, &d1, prev1), ALP_OK);
    uint8_t expect[32]; ulog_sha256(e0, n0, expect);
    zassert_mem_equal(prev1, expect, 32);
    zassert_equal(d1.seq, 1);
}
```
Add `#include "../../../../src/update_log/sha256.h"` to the test includes.

- [ ] **Step 3: Run — verify it fails**
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
```
Expected: FAIL — `ulog_engine_append` undefined.

- [ ] **Step 4: Implement `ulog_engine_append`** in `src/update_log/engine.c`:
```c
static void kbuf(char *out, size_t cap, uint64_t seq) {
    /* "ulog.<seq>" decimal. */
    int n = 0; char tmp[24]; uint64_t v = seq;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < 20);
    size_t pos = 0; const char *pfx = "ulog.";
    while (*pfx && pos < cap-1) out[pos++] = *pfx++;
    while (n > 0 && pos < cap-1) out[pos++] = tmp[--n];
    out[pos] = 0;
}

alp_status_t ulog_engine_append(const alp_secure_store_if *store,
                                const alp_monotonic_counter_if *ctr,
                                const alp_update_log_entry_t *entry)
{
    if (store == NULL || ctr == NULL || entry == NULL) return ALP_ERR_INVAL;

    uint64_t hw = 0;
    alp_status_t rc = ctr->read(ctr->ctx, 0, &hw);
    if (rc != ALP_OK) return rc;

    uint8_t prev[32]; memset(prev, 0, sizeof(prev));
    if (hw > 0) {
        uint8_t metabuf[ULOG_META_WIRE_LEN]; size_t mlen = 0;
        rc = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
        if (rc != ALP_OK) return rc;
        struct ulog_meta m;
        rc = ulog_meta_decode(metabuf, mlen, &m);
        if (rc != ALP_OK) return rc;
        memcpy(prev, m.head_hash, 32);
    }

    alp_update_log_entry_t e = *entry; e.seq = hw;
    uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    rc = ulog_entry_encode(&e, prev, wire);
    if (rc != ALP_OK) return rc;

    char key[24]; kbuf(key, sizeof(key), hw);
    rc = store->put(store->ctx, key, wire, sizeof(wire));
    if (rc != ALP_OK) return rc;

    struct ulog_meta nm; nm.count = hw + 1u;
    ulog_sha256(wire, sizeof(wire), nm.head_hash);
    uint8_t metaout[ULOG_META_WIRE_LEN];
    (void)ulog_meta_encode(&nm, metaout);
    rc = store->put(store->ctx, "ulog.meta", metaout, sizeof(metaout));
    if (rc != ALP_OK) return rc;

    uint64_t newhw = 0;
    return ctr->increment(ctr->ctx, 0, &newhw);
}
```
Add `#include "update_log/sha256.h"` to engine.c includes (already added in Task 1 Step 5).

- [ ] **Step 5: Run — verify pass; commit**
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
git add src/update_log/engine.c tests/unit/update_log/src/test_update_log.c
git commit -q -m "feat(update-log): hash-chained append over the engine seams"
```

---

## Task 3: verify() — happy path + all four tamper classes

**Files:**
- Modify: `src/update_log/engine.c` (add `ulog_engine_verify`)
- Test: `tests/unit/update_log/src/test_update_log.c`

- [ ] **Step 1: Failing tests (happy + 4 tamper classes)**
```c
static void seed(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *c, int n) {
    td_ifaces(t, s, c);
    for (int i = 0; i < n; i++)
        (void)ulog_engine_append(s, c, &mk_entry((uint64_t)(i+1)*10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED));
}

ZTEST(alp_update_log, test_verify_ok)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; seed(&t,&s,&c,3);
    alp_update_log_verdict_t v; uint64_t bad = 999;
    zassert_equal(ulog_engine_verify(&s,&c,&v,&bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log, test_verify_detects_mutation)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; seed(&t,&s,&c,3);
    int i = td_find(&t, "ulog.1"); t.s[i].buf[12] ^= 0xFF;   /* flip a timestamp byte in entry 1 */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s,&c,&v,&bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
    zassert_equal(bad, 2);   /* entry 2's prev_hash no longer matches mutated entry 1 */
}

ZTEST(alp_update_log, test_verify_detects_truncation)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; seed(&t,&s,&c,3);
    (void)td_erase(&t, "ulog.2");   /* drop the tail; counter still says 3 */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s,&c,&v,&bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_TRUNCATED);
}

ZTEST(alp_update_log, test_verify_detects_rollback)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; seed(&t,&s,&c,3);
    t.counter = 5;   /* counter advanced past the stored meta.count (=3): store regressed */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s,&c,&v,&bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
}

ZTEST(alp_update_log, test_verify_detects_reorder)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; seed(&t,&s,&c,3);
    int a = td_find(&t,"ulog.0"), b = td_find(&t,"ulog.1");
    uint8_t tmp[TD_BLOB]; memcpy(tmp, t.s[a].buf, TD_BLOB);
    memcpy(t.s[a].buf, t.s[b].buf, TD_BLOB); memcpy(t.s[b].buf, tmp, TD_BLOB);  /* swap contents */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s,&c,&v,&bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
}
```

- [ ] **Step 2: Run — verify it fails** (`ulog_engine_verify` undefined).

- [ ] **Step 3: Implement `ulog_engine_verify`** in `src/update_log/engine.c`:
```c
alp_status_t ulog_engine_verify(const alp_secure_store_if *store,
                                const alp_monotonic_counter_if *ctr,
                                alp_update_log_verdict_t *verdict_out,
                                uint64_t *bad_seq_out)
{
    if (store == NULL || ctr == NULL || verdict_out == NULL) return ALP_ERR_INVAL;
    if (bad_seq_out) *bad_seq_out = 0;

    uint64_t hw = 0;
    alp_status_t rc = ctr->read(ctr->ctx, 0, &hw);
    if (rc != ALP_OK) return ALP_ERR_IO;

    struct ulog_meta m; m.count = 0; memset(m.head_hash, 0, 32);
    if (hw > 0) {
        uint8_t metabuf[ULOG_META_WIRE_LEN]; size_t mlen = 0;
        rc = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
        if (rc == ALP_ERR_NOT_FOUND) { *verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK; return ALP_OK; }
        if (rc != ALP_OK) return ALP_ERR_IO;
        if (ulog_meta_decode(metabuf, mlen, &m) != ALP_OK) return ALP_ERR_IO;
        /* The counter is the trusted anchor. If the stored meta disagrees, the
         * store (or the counter) was rolled back. */
        if (m.count != hw) { *verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK; return ALP_OK; }
    }

    uint8_t prev[32]; memset(prev, 0, sizeof(prev));
    uint8_t cur_hash[32]; memset(cur_hash, 0, sizeof(cur_hash));
    for (uint64_t i = 0; i < hw; i++) {
        char key[24]; kbuf(key, sizeof(key), i);
        uint8_t wire[ULOG_ENTRY_WIRE_LEN]; size_t n = 0;
        rc = store->get(store->ctx, key, wire, sizeof(wire), &n);
        if (rc == ALP_ERR_NOT_FOUND) { *verdict_out = ALP_UPDATE_LOG_VERIFY_TRUNCATED;
                                       if (bad_seq_out) *bad_seq_out = i; return ALP_OK; }
        if (rc != ALP_OK) return ALP_ERR_IO;

        alp_update_log_entry_t e; uint8_t got_prev[32];
        if (ulog_entry_decode(wire, n, &e, got_prev) != ALP_OK) {
            *verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN; if (bad_seq_out) *bad_seq_out = i; return ALP_OK; }
        if (e.seq != i || memcmp(got_prev, prev, 32) != 0) {
            *verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN; if (bad_seq_out) *bad_seq_out = i; return ALP_OK; }
        ulog_sha256(wire, n, cur_hash);
        memcpy(prev, cur_hash, 32);
    }

    if (hw > 0 && memcmp(cur_hash, m.head_hash, 32) != 0) {
        *verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN; if (bad_seq_out) *bad_seq_out = hw - 1; return ALP_OK; }

    *verdict_out = ALP_UPDATE_LOG_VERIFY_OK;
    return ALP_OK;
}
```

- [ ] **Step 4: Run — verify pass (all 6 verify tests green); commit**
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
git add src/update_log/engine.c tests/unit/update_log/src/test_update_log.c
git commit -q -m "feat(update-log): verify() with mutation/truncation/rollback/reorder detection"
```

---

## Task 4: count() + get() (and carry image_hash on the wire)

**Files:**
- Modify: `include/alp/update_log.h` (widen entry wire to include image_hash)
- Modify: `src/update_log/engine.{h,c}` (`ULOG_ENTRY_WIRE_LEN` 83→115; encode/decode image_hash; add count/get)
- Test: `tests/unit/update_log/src/test_update_log.c`

- [ ] **Step 1: Widen the wire format to carry `image_hash`**

In `src/update_log/engine.h` change `#define ULOG_ENTRY_WIRE_LEN 83u` → `115u`
(83 + 32 for image_hash appended at offset 83).

In `src/update_log/engine.c`, in `ulog_entry_encode` add before `return ALP_OK;`:
```c
    memcpy(out + 83, e->image_hash, 32);
```
and in `ulog_entry_decode` add before `return ALP_OK;`:
```c
    memcpy(e_out->image_hash, buf + 83, 32);
```
(Delete the Task-1 "image_hash is NOT on the wire" comment in decode.)

- [ ] **Step 2: Failing count/get test**
```c
ZTEST(alp_update_log, test_count_and_get)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c; td_ifaces(&t,&s,&c);
    (void)ulog_engine_append(&s,&c,&mk_entry(10,"a",ALP_UPDATE_STATUS_CONFIRMED));
    (void)ulog_engine_append(&s,&c,&mk_entry(20,"b",ALP_UPDATE_STATUS_ROLLED_BACK));

    uint64_t n = 0; zassert_equal(ulog_engine_count(&s,&c,&n), ALP_OK); zassert_equal(n, 2);

    alp_update_log_entry_t e; zassert_equal(ulog_engine_get(&s, 1, &e), ALP_OK);
    zassert_equal(e.seq, 1); zassert_equal(e.status, ALP_UPDATE_STATUS_ROLLED_BACK);
    zassert_equal(strcmp(e.fw_version, "b"), 0);
    uint8_t want[32]; memset(want, 20, 32); zassert_mem_equal(e.image_hash, want, 32);

    zassert_equal(ulog_engine_get(&s, 9, &e), ALP_ERR_NOT_FOUND);
}
```

- [ ] **Step 3: Run — verify it fails.**

- [ ] **Step 4: Implement count/get** in `src/update_log/engine.c`:
```c
alp_status_t ulog_engine_count(const alp_secure_store_if *store,
                               const alp_monotonic_counter_if *ctr, uint64_t *count_out)
{
    (void)store;
    if (ctr == NULL || count_out == NULL) return ALP_ERR_INVAL;
    return ctr->read(ctr->ctx, 0, count_out);
}

alp_status_t ulog_engine_get(const alp_secure_store_if *store, uint64_t seq,
                             alp_update_log_entry_t *e_out)
{
    if (store == NULL || e_out == NULL) return ALP_ERR_INVAL;
    char key[24]; kbuf(key, sizeof(key), seq);
    uint8_t wire[ULOG_ENTRY_WIRE_LEN]; size_t n = 0;
    alp_status_t rc = store->get(store->ctx, key, wire, sizeof(wire), &n);
    if (rc != ALP_OK) return rc;   /* ALP_ERR_NOT_FOUND propagates */
    uint8_t prev[32];
    return ulog_entry_decode(wire, n, e_out, prev);
}
```

- [ ] **Step 5: Run — verify all green (also re-run Tasks 1-3 tests; the wire widen must not break them); commit**
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
git add include/alp/update_log.h src/update_log/engine.h src/update_log/engine.c tests/unit/update_log/src/test_update_log.c
git commit -q -m "feat(update-log): count()/get() + carry image_hash on the wire"
```

---

## Task 5: Software tier backend, ops vtable, dispatch + public API

**Files:**
- Create: `src/backends/update_log/update_log_ops.h`
- Create: `src/backends/update_log/sw_tier.c`
- Create: `src/backends/update_log/alif_tfm_stub.c`
- Create: `src/update_log_dispatch.c`
- Modify: `zephyr/CMakeLists.txt` (append the four files from Task 1 Step 7)
- Test: `tests/unit/update_log/src/test_update_log.c` (public-surface smoke)

- [ ] **Step 1: Ops vtable + backend state**

`src/backends/update_log/update_log_ops.h`:
```c
/* SPDX-License-Identifier: Apache-2.0  -- internal, NOT public. */
#ifndef ALP_BACKENDS_UPDATE_LOG_OPS_H
#define ALP_BACKENDS_UPDATE_LOG_OPS_H

#include <stdbool.h>
#include "alp/backend.h"
#include "alp/update_log.h"

typedef struct alp_update_log_ops {
    alp_update_log_assurance_t assurance;
    alp_status_t (*append)(const alp_update_log_entry_t *e);
    alp_status_t (*verify)(alp_update_log_verdict_t *v, uint64_t *bad);
    alp_status_t (*count) (uint64_t *out);
    alp_status_t (*get)   (uint64_t seq, alp_update_log_entry_t *out);
} alp_update_log_ops_t;

/* Dispatcher-owned handle. Single instance (one log per device). */
struct alp_update_log {
    const alp_update_log_ops_t *ops;
    alp_update_log_assurance_t  assurance;
    bool                        in_use;
};

#endif
```

- [ ] **Step 2: Software tier backend**

`src/backends/update_log/sw_tier.c`:
```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * Software tamper-evident tier for <alp/update_log.h>. The universal
 * silicon_ref="*" fallback: works on every SoM, reports assurance
 * SW_TAMPER_EVIDENT. Backs the engine with a RAM store + a RAM monotonic
 * counter.
 *
 * SLICE 1 SCOPE: the store is in-RAM, so the log does not survive reboot.
 * Durable persistence via Zephyr Settings/NVS is the named follow-up before
 * this tier is production. The engine + assurance contract are unchanged by
 * that swap -- only the store_if implementation gains persistence.
 */
#include <string.h>

#include "alp/backend.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"
#include "update_log/engine.h"
#include "update_log/store.h"

#define SW_SLOTS 32
#define SW_BLOB  ULOG_ENTRY_WIRE_LEN
struct sw_state {
    struct { char key[24]; uint8_t buf[SW_BLOB]; size_t len; bool used; } s[SW_SLOTS];
    uint64_t counter;
};
static struct sw_state g_sw;

static int sw_find(const char *key) {
    for (int i=0;i<SW_SLOTS;i++) if (g_sw.s[i].used && strcmp(g_sw.s[i].key,key)==0) return i; return -1; }
static alp_status_t sw_put(void *c,const char *k,const uint8_t *b,size_t n){ (void)c;
    if (n>SW_BLOB) return ALP_ERR_NOMEM; int i=sw_find(k);
    if (i<0){for(i=0;i<SW_SLOTS;i++) if(!g_sw.s[i].used) break; if(i==SW_SLOTS) return ALP_ERR_NOMEM;}
    g_sw.s[i].used=true; strncpy(g_sw.s[i].key,k,sizeof(g_sw.s[i].key)-1);
    g_sw.s[i].key[sizeof(g_sw.s[i].key)-1]=0; memcpy(g_sw.s[i].buf,b,n); g_sw.s[i].len=n; return ALP_OK; }
static alp_status_t sw_get(void *c,const char *k,uint8_t *b,size_t cap,size_t *o){ (void)c;
    int i=sw_find(k); if(i<0) return ALP_ERR_NOT_FOUND; if(g_sw.s[i].len>cap) return ALP_ERR_NOMEM;
    memcpy(b,g_sw.s[i].buf,g_sw.s[i].len); if(o)*o=g_sw.s[i].len; return ALP_OK; }
static alp_status_t sw_erase(void *c,const char *k){ (void)c; int i=sw_find(k);
    if(i<0) return ALP_ERR_NOT_FOUND; g_sw.s[i].used=false; return ALP_OK; }
static alp_status_t sw_cread(void *c,uint32_t id,uint64_t *v){ (void)c;(void)id; *v=g_sw.counter; return ALP_OK; }
static alp_status_t sw_cinc(void *c,uint32_t id,uint64_t *v){ (void)c;(void)id; g_sw.counter++; *v=g_sw.counter; return ALP_OK; }

static const alp_secure_store_if g_store = { sw_put, sw_get, sw_erase, NULL };
static const alp_monotonic_counter_if g_ctr = { sw_cread, sw_cinc, NULL };

static alp_status_t sw_append(const alp_update_log_entry_t *e){ return ulog_engine_append(&g_store,&g_ctr,e); }
static alp_status_t sw_verify(alp_update_log_verdict_t *v,uint64_t *bad){ return ulog_engine_verify(&g_store,&g_ctr,v,bad); }
static alp_status_t sw_count(uint64_t *o){ return ulog_engine_count(&g_store,&g_ctr,o); }
static alp_status_t sw_get_e(uint64_t seq,alp_update_log_entry_t *o){ return ulog_engine_get(&g_store,seq,o); }

static const alp_update_log_ops_t _sw_ops = {
    .assurance = ALP_UPDATE_LOG_SW_TAMPER_EVIDENT,
    .append = sw_append, .verify = sw_verify, .count = sw_count, .get = sw_get_e,
};

ALP_BACKEND_REGISTER(update_log, sw_tier, {
    .silicon_ref = "*",
    .vendor      = "sw_tier",
    .base_caps   = 0u,
    .priority    = 10,
    .ops         = &_sw_ops,
    .probe       = NULL,
});
```

- [ ] **Step 3: Alif TF-M stub seam (compiled, unregistered)**

`src/backends/update_log/alif_tfm_stub.c`:
```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * Alif TF-M hardware-enforced tier for <alp/update_log.h> -- SEAM ONLY.
 *
 * This file documents and reserves the shape of the HW_ENFORCED backend:
 *   - store_if  -> TF-M PSA Protected Storage (psa_ps_set/get), an asset in
 *     the Secure Processing Environment unreachable by the non-secure app.
 *   - counter   -> a hardware monotonic counter (PSA NV counter or OPTIGA),
 *     non-decrementable.
 *   - assurance -> ALP_UPDATE_LOG_HW_ENFORCED.
 *
 * It is COMPILED (so it cannot bitrot) but deliberately NOT registered via
 * ALP_BACKEND_REGISTER: until the Alif board file + TF-M build land and the
 * acceptance criteria in the design spec pass on silicon, the software tier
 * serves every SoM. Wiring this up is a later slice. Every entry point
 * returns ALP_ERR_NOSUPPORT.
 */
#include "alp/update_log.h"
#include "update_log/store.h"

alp_status_t alp_update_log_alif_tfm_store_put(void *c, const char *k, const uint8_t *b, size_t n)
{ (void)c;(void)k;(void)b;(void)n; return ALP_ERR_NOSUPPORT; }
alp_status_t alp_update_log_alif_tfm_counter_read(void *c, uint32_t id, uint64_t *v)
{ (void)c;(void)id;(void)v; return ALP_ERR_NOSUPPORT; }
```

- [ ] **Step 4: Dispatch + public API**

`src/update_log_dispatch.c`:
```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * <alp/update_log.h> dispatcher. One log per device: a single static handle
 * bound to the selected backend (the sw_tier wildcard today; a HW_ENFORCED
 * backend later). Mirrors the TMU stateless-cache pattern -- the surface is
 * effectively a singleton, so we cache the chosen ops on first open().
 */
#include <stdbool.h>
#include <stddef.h>

#include "alp/backend.h"
#include "alp/peripheral.h"
#include "alp/soc_caps.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"

ALP_BACKEND_DEFINE_CLASS(update_log);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

static struct alp_update_log g_log;

alp_update_log_t *alp_update_log_open(void)
{
    alp_z_clear_last_error();
    if (g_log.in_use) return &g_log;
    const alp_backend_t *be = alp_backend_select("update_log", ALP_SOC_REF_STR);
    if (be == NULL || be->ops == NULL) { alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC); return NULL; }
    g_log.ops       = (const alp_update_log_ops_t *)be->ops;
    g_log.assurance = g_log.ops->assurance;
    g_log.in_use    = true;
    return &g_log;
}

alp_status_t alp_update_log_append(alp_update_log_t *log, const alp_update_log_entry_t *entry)
{
    if (log == NULL || !log->in_use || entry == NULL) return ALP_ERR_INVAL;
    if (log->ops->append == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return log->ops->append(entry);
}

alp_status_t alp_update_log_verify(alp_update_log_t *log, alp_update_log_verdict_t *verdict_out,
                                   uint64_t *bad_seq_out)
{
    if (log == NULL || !log->in_use || verdict_out == NULL) return ALP_ERR_INVAL;
    if (log->ops->verify == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return log->ops->verify(verdict_out, bad_seq_out);
}

alp_status_t alp_update_log_count(alp_update_log_t *log, uint64_t *count_out)
{
    if (log == NULL || !log->in_use || count_out == NULL) return ALP_ERR_INVAL;
    if (log->ops->count == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return log->ops->count(count_out);
}

alp_status_t alp_update_log_get(alp_update_log_t *log, uint64_t seq, alp_update_log_entry_t *entry_out)
{
    if (log == NULL || !log->in_use || entry_out == NULL) return ALP_ERR_INVAL;
    if (log->ops->get == NULL) return ALP_ERR_NOT_IMPLEMENTED;
    return log->ops->get(seq, entry_out);
}

alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log)
{
    return (log != NULL) ? log->assurance : ALP_UPDATE_LOG_SW_TAMPER_EVIDENT;
}

void alp_update_log_close(alp_update_log_t *log) { if (log != NULL) log->in_use = false; }
```

- [ ] **Step 5: Append the remaining sources to the CMake block** (the Task 1 Step 7 block now lists all five files).

- [ ] **Step 6: Public-surface smoke test**
```c
ZTEST(alp_update_log, test_public_surface_sw_tier)
{
    alp_update_log_t *log = alp_update_log_open();
    zassert_not_null(log);
    zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_SW_TAMPER_EVIDENT);

    alp_update_log_entry_t e = mk_entry(42, "2.0.0", ALP_UPDATE_STATUS_PENDING_CONFIRM);
    zassert_equal(alp_update_log_append(log, &e), ALP_OK);

    uint64_t n = 0; zassert_equal(alp_update_log_count(log, &n), ALP_OK); zassert_true(n >= 1);
    alp_update_log_verdict_t v;
    zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
    alp_update_log_close(log);
}
```
> The sw_tier store is a module-global; if a later test depends on a clean
> store, add a reset hook. For slice 1 keep this test last in the file.

- [ ] **Step 7: Run — verify pass; commit**
```
west twister -p native_sim/native/64 -T tests/unit/update_log --inline-logs
git add src/backends/update_log/ src/update_log_dispatch.c zephyr/CMakeLists.txt tests/unit/update_log/src/test_update_log.c
git commit -q -m "feat(update-log): sw_tier backend + registry dispatch + public API"
```

---

## Task 6: Fuzz the entry decode path

**Files:**
- Create: `tests/fuzz/update_log_entry_fuzz.c`
- Create: `tests/fuzz/corpus/update_log_entry/.gitkeep`
- Modify: `tests/fuzz/CMakeLists.txt`

- [ ] **Step 1: Harness** `tests/fuzz/update_log_entry_fuzz.c`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the update-log entry/meta decode path -- the code
 * that ingests attacker-reachable bytes from persistent storage. Links the
 * REAL engine (engine.c + sha256.c) so it fuzzes production decode, not a
 * mirror.
 *
 * Build:  cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal -DCMAKE_C_COMPILER=clang
 *         cmake --build build-fuzz --target alp_fuzz_update_log_entry
 * Run:    ./build-fuzz/tests/fuzz/alp_fuzz_update_log_entry -max_total_time=30 \
 *               tests/fuzz/corpus/update_log_entry
 */
#include <stddef.h>
#include <stdint.h>

#include "update_log/engine.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    alp_update_log_entry_t e; uint8_t prev[32];
    (void)ulog_entry_decode(data, size, &e, prev);
    struct ulog_meta m;
    (void)ulog_meta_decode(data, size, &m);
    return 0;
}
```

- [ ] **Step 2: Wire the target** — in `tests/fuzz/CMakeLists.txt`, add after the existing `_alp_add_fuzz_target(...)` calls:
```cmake
_alp_add_fuzz_target(update_log_entry)
# Link the real engine + vendored SHA-256 so the harness fuzzes production decode.
target_sources(alp_fuzz_update_log_entry PRIVATE
    ${CMAKE_SOURCE_DIR}/src/update_log/engine.c
    ${CMAKE_SOURCE_DIR}/src/update_log/sha256.c)
target_include_directories(alp_fuzz_update_log_entry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include)
```
Create `tests/fuzz/corpus/update_log_entry/.gitkeep` (empty).

- [ ] **Step 3: Build the target — verify it compiles + runs briefly**
```
cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz --target alp_fuzz_update_log_entry
./build-fuzz/tests/fuzz/alp_fuzz_update_log_entry -max_total_time=20 tests/fuzz/corpus/update_log_entry
```
Expected: builds clean; fuzzer runs 20 s with no ASan/UBSan crash. (Clang required; on Windows run under WSL.)

- [ ] **Step 4: Commit**
```
git add tests/fuzz/update_log_entry_fuzz.c tests/fuzz/CMakeLists.txt tests/fuzz/corpus/update_log_entry/.gitkeep
git commit -q -m "test(update-log): fuzz the entry/meta decode path against the real engine"
```

---

## Task 7: native_sim example + README

**Files:**
- Create: `examples/connectivity/firmware-update-log/{CMakeLists.txt,prj.conf,board.yaml,generated/native_sim.conf,testcase.yaml,src/main.c,README.md}`

- [ ] **Step 1: Copy the example scaffolding from an AEN-targeted example.**

Copy `board.yaml` and `CMakeLists.txt` verbatim from
`examples/connectivity/production-deployment/` into the new
`examples/connectivity/firmware-update-log/` (do NOT hand-author SoM metadata —
reuse the existing valid AEN `som:` block; only change the cmake `project()`
name to `firmware_update_log`).

- [ ] **Step 2: prj.conf**
```
CONFIG_PRINTK=y
CONFIG_LOG=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SDK_UPDATE_LOG=y
```

- [ ] **Step 3: generated/native_sim.conf** (no extra Kconfig needed — the SW tier
needs no hardware; the file exists to match the overlay convention and to be the
hook for any future native_sim-only tweak):
```
# SPDX-License-Identifier: Apache-2.0
# native_sim overlay for firmware-update-log. The software tier needs no
# peripheral; this file exists so the example follows the generated/ overlay
# convention (escapes Zephyr's ${APPLICATION_BINARY_DIR}/*.conf GLOB).
CONFIG_PRINTK=y
```

- [ ] **Step 4: src/main.c** (teaching artifact, ~50% comments per the examples convention):
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-update-log -- portable, tamper-evident update audit log.
 *
 * This is the ONE API a customer uses regardless of SoM. On a SoM with a
 * secure backend the very same code is hardware-enforced (TF-M Protected
 * Storage + a monotonic counter); here on native_sim it runs the software
 * tier. The assurance readout tells you which you got -- so the app can log
 * or branch on the guarantee level without ever naming a vendor mechanism.
 *
 * Flow: open the log, append one update record (as MCUboot/a secure service
 * would after verifying an image), then verify the whole chain and print it.
 */
#include <stdio.h>
#include <string.h>

#include <alp/update_log.h>

static const char *assurance_str(alp_update_log_assurance_t a)
{
    return (a == ALP_UPDATE_LOG_HW_ENFORCED) ? "HW_ENFORCED (TF-M isolated)"
                                             : "SW_TAMPER_EVIDENT (software tier)";
}

static const char *verdict_str(alp_update_log_verdict_t v)
{
    switch (v) {
    case ALP_UPDATE_LOG_VERIFY_OK:           return "OK";
    case ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN: return "CHAIN_BROKEN";
    case ALP_UPDATE_LOG_VERIFY_TRUNCATED:    return "TRUNCATED";
    case ALP_UPDATE_LOG_VERIFY_ROLLED_BACK:  return "ROLLED_BACK";
    default:                                 return "?";
    }
}

int main(void)
{
    /* Open the device's update log. NULL means no backend on this SoM. */
    alp_update_log_t *log = alp_update_log_open();
    if (log == NULL) { printf("[update-log] no backend present\n"); return 0; }

    /* Tell the operator exactly how strong the guarantee is on this silicon. */
    printf("[update-log] assurance: %s\n", assurance_str(alp_update_log_assurance(log)));

    /* Record an update result. In production the bootloader/secure service
     * fills these in after verifying the image it just installed. */
    alp_update_log_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.fw_version, "1.4.2", ALP_UPDATE_LOG_FWVER_MAX);
    memset(e.image_hash, 0x5A, sizeof(e.image_hash));  /* SHA-256 of the image */
    e.status    = ALP_UPDATE_STATUS_CONFIRMED;
    e.timestamp = 1718000000u;                         /* best-effort epoch */

    if (alp_update_log_append(log, &e) != ALP_OK) {
        printf("[update-log] append failed\n"); return 0;
    }

    /* Verify the whole chain -- detects mutation/truncation/rollback/reorder
     * of any historical entry (software-detectable in this tier; hardware-
     * enforced where the secure backend is present). */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    if (alp_update_log_verify(log, &v, &bad) == ALP_OK) {
        printf("[update-log] verify: %s\n", verdict_str(v));
    }

    /* Print the append-only trail. */
    uint64_t n = 0;
    if (alp_update_log_count(log, &n) == ALP_OK) {
        printf("[update-log] %llu entr%s:\n", (unsigned long long)n, (n == 1) ? "y" : "ies");
        for (uint64_t i = 0; i < n; i++) {
            alp_update_log_entry_t r;
            if (alp_update_log_get(log, i, &r) == ALP_OK) {
                printf("  #%llu  v=%s  status=%d  ts=%llu\n",
                       (unsigned long long)r.seq, r.fw_version, (int)r.status,
                       (unsigned long long)r.timestamp);
            }
        }
    }

    alp_update_log_close(log);
    return 0;
}
```

- [ ] **Step 5: testcase.yaml**
```yaml
# SPDX-License-Identifier: Apache-2.0
sample:
  name: firmware-update-log
  description: |
    Portable tamper-evident firmware-update audit log via
    <alp/update_log.h>. Software tier runs on native_sim; the same
    code is hardware-enforced where a secure backend is present.
tests:
  alp_sdk.examples.connectivity.firmware_update_log.native_sim:
    platform_allow:
      - native_sim/native/64
    extra_args: EXTRA_CONF_FILE=generated/native_sim.conf
    tags:
      - alp-sdk
      - example
      - update_log
      - security
    harness: console
    harness_config:
      type: one_line
      regex:
        - "verify: OK"
```

- [ ] **Step 6: README.md** — short teaching doc. Lead with the assurance-level
table (SW_TAMPER_EVIDENT vs HW_ENFORCED), the "one API across SoMs" point, and a
note that the slice-1 software store is in-RAM (durable persistence is the named
follow-up). Link the design spec.

- [ ] **Step 7: Run — verify it builds + the console regex matches; commit**
```
west twister -p native_sim/native/64 -T examples/connectivity/firmware-update-log --inline-logs
git add examples/connectivity/firmware-update-log/
git commit -q -m "docs(update-log): native_sim example demonstrating the portable surface"
```

---

## Task 8: Docs sync + ABI marker registration

**Files:**
- Modify: `docs/abi-markers.md` (register the new experimental surface)
- Modify: `docs/threat-model.md` (add an `<alp/update_log.h>` row)
- Modify: `CHANGELOG.md` (Added entry)

- [ ] **Step 1:** In `docs/abi-markers.md`, add `<alp/update_log.h>` to the
`[ABI-EXPERIMENTAL]` list with a one-line note ("v0.7 new; experimental until the
hardware-enforced backend is silicon-proven").

- [ ] **Step 2:** In `docs/threat-model.md` §4 table, add:
`| <alp/update_log.h> | Tamper of historical update records | Hash-chain + monotonic-counter (SW tier, tamper-evident); TF-M Protected Storage + HW counter (HW_ENFORCED tier) |`

- [ ] **Step 3:** Add a `CHANGELOG.md` "Added" entry under the current unreleased
section describing the experimental surface + the SW tier + the assurance level.

- [ ] **Step 4:** Run the repo doc-lint + the full update_log suite once more, then commit:
```
west twister -p native_sim/native/64 -T tests/unit/update_log -T examples/connectivity/firmware-update-log --inline-logs
git add docs/abi-markers.md docs/threat-model.md CHANGELOG.md
git commit -q -m "docs(update-log): register experimental ABI + threat-model row + changelog"
```

---

## Final gate (before opening the PR)

- [ ] Run the full local CI per `running-local-ci` / `docs/local-ci.md`: the
  native_sim twister scope (`tests/unit` + `examples`), clang-format diff, and
  the doc-lint. All green.
- [ ] `propagating-code-changes` self-check: a new public header was added —
  confirm no dispatch/registry/caps drift (the surface is intentionally NOT in
  the caps matrix; verify the build links on an AEN-pinned and a native_sim
  config).
- [ ] Push the branch and open a PR `--base dev` (0-approval, self-merge once
  green) per `starting-work-on-a-branch`.

---

## Self-Review (completed at authoring)

- **Spec coverage:** portable surface (Tasks 1,5) ✓; software tier first-class
  (Task 5) ✓; assurance level (Tasks 1,5) ✓; hardware seam stub (Task 5 Step 3) ✓;
  entry format incl. version/hash/status/timestamp (Tasks 1,4) ✓; tamper model —
  mutation/truncation/rollback/reorder (Task 3) ✓; experimental ABI (Tasks 1,8) ✓;
  no caps-matrix/security.psa (build wiring + final gate) ✓; fuzz (Task 6) ✓;
  example (Task 7) ✓; HW_ENFORCED acceptance criteria — documented in the spec,
  not implemented this slice (correct) ✓.
- **Placeholder scan:** SHA-256 is a verbatim vendored public-domain file (flagged,
  justified); `board.yaml` is copied from an existing example to avoid fabricating
  SoM metadata (flagged). No other TBD/TODO.
- **Type consistency:** `alp_update_log_*` names, `ULOG_ENTRY_WIRE_LEN` (83→115 in
  Task 4, both call sites updated), `ulog_engine_*` signatures, and the seam
  struct field names match across Tasks 1-7. `kbuf()` defined in Task 2, reused in
  Tasks 3-4. The wire-widen in Task 4 touches exactly `engine.h` + the two
  encode/decode bodies.
