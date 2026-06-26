# BRD_I2C Driver Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every IC driver on the V2N SoM's BRD_I2C bus silicon-ready: fix the two unverified register maps (act8760, da9292), add mocked-I2C unit tests for all BRD_I2C chip drivers, and ship a probe-all bring-up example to flash on hardware-patch day.

**Architecture:** Three independent streams sharing one enabler. The chips test suite's i2c-emul fakes are currently disabled (a Zephyr 3.7-era link issue; we're pinned v4.4.0 now) — Task 1 re-enables them, Task 2 adds a shared 8-bit register-file emulator core, then per-chip tasks add fakes + register-level ztests. The act8760 driver gets its register map replaced wholesale (the old offsets were guesses; verified truth extracted 2026-06-06 from the AA82BZ register-map workbook + ACT88760 Datasheet Rev C and adversarially re-derived). The da9292 driver's guessed layout was verified CORRECT against DA9292 Datasheet Rev 2.2 — it only needs citations. The example mirrors `examples/v2n/v2n-rtc-multi-alarm/` conventions exactly.

**Tech Stack:** Zephyr v4.4.0 ztest + i2c-emul on `native_sim/native/64`, alp portable I2C API (`<alp/peripheral.h>`), chip drivers under `chips/<part>/`, twister via WSL.

**Verified register facts (do not re-derive; citations in task bodies):**

| Fact | Value |
|---|---|
| ACT8760 addressing | TWO independent slaves: ADD1 (0x25 on CMI 120.E1) = MSTR + GPIO + Buck1–6; ADD2 (0x26) = Buck7 + LDO1–6. All four documented CMI address pairs are ADD2 = ADD1+1. |
| ACT8760 VSET0 regs | ADD1: B1 0x42, B2 0x62, B3 0x82, B4 0xA2, B5 0xC2, B6 0xE2 · ADD2: B7 **0x02**, LDO1 0x21, LDO2 0x27, LDO3 0x47, LDO4 0x67, LDO5 0x41, LDO6 0x61 |
| ACT8760 VSET fields | Bucks: bits[6:0] (mask 0x7F), bit7 = EN_OutPD / IPD_SET (live control — preserve on write). LDOs: bits[5:0] (mask 0x3F), RANGE bit above (bit6 LDO1/2, bit7 LDO3–6). |
| ACT8760 reg 0x00 (ADD1) | bit7 ROM_STAT, bit6 WD_TIMER_ALERT, **bit5 TWARN**, bit4 VSYSSTAT (latched), bit3 VIN_POK_OV, bit2 PBASTAT, bit1 VSYSWARN (latched), bit0 PBDSTAT. SYSDAT is in reg 0x02 bit4, NOT 0x00. |
| ACT8760 VSET→mV | Bucks: VOUT = 500 mV + VSET×5 mV (Output-Low) or ×25 mV (Output-High; Buck5/6 high-only). LDO1/2: 0.5–1.2875 V / 1.2–1.9875 V, 12.5 mV. LDO3–6: 0.5–1.2875 V / 1–4.15 V. |
| DA9292 STATUS_00 | bit7..0 = S_CH2_OC, S_CH1_OC, S_CH2_OV, S_CH1_OV, S_CH2_UV, S_CH1_UV, S_CH2_PG, S_CH1_PG — **the driver's existing defines are CORRECT** (Table 14 p.36–37, Rev 2.2). |
| DA9292 others | EVENT_00/01 same layout, RWC1 (Tables 16–17); MASK_00 reset 0xFF, MASK_01 reset 0x07; CTRL_01 = VSTEP[7:6] DIS_PD[5:4] VSEL[3:2] EN[1:0] (Table 21); VOUT: byte 0x3C = 300 mV, 5 mV/LSB, max 0xFF = 1275 mV, desc-table reset 0xA3 = 815 mV (Table 24); VSTEP only writable while CH_EN=0 (§5.1 Note 1 p.22); DEV_ID reset 0xEA, REV_ID 0x10 (Table 12 p.35). All driver constants verified correct. |

**Known discrepancy to surface during bring-up (do NOT silently fix):** `metadata/e1m_modules/E1M-V2N101.yaml` puts the TMP112 at **0x40**, but the TMP112 datasheet (and the driver's range check) only allows 0x48–0x4B. The example probes both and reports; the metadata fix happens only after silicon confirms.

---

### Task 1: Re-enable the i2c-emul fake infrastructure under Zephyr v4.4

The three existing fakes (`fake_lsm6dso/ssd1306/bme280`) were commented out for a Zephyr **3.7** native_sim link error (`__device_dts_ord_<N>` unresolved for `EMUL_DT_INST_DEFINE` children). The pin moved to v4.4.0; the emul framework matured. Re-enable and prove the link.

**Files:**
- Modify: `tests/zephyr/chips/CMakeLists.txt`
- Modify: `tests/zephyr/chips/boards/native_sim_native_64.overlay`

- [ ] **Step 1: Uncomment the fake sources in CMakeLists.txt**

Replace lines 10–22 of `tests/zephyr/chips/CMakeLists.txt` (the whole comment block + commented target_sources) with:

```cmake
# Fake i2c-emul fixtures.  Each fake_<chip>.c registers an
# EMUL_DT_INST_DEFINE target under the i2c0_emul controller (see
# boards/native_sim_native_64.overlay) so the chip drivers exercise
# real register transfers on native_sim.
target_sources(app PRIVATE
    src/fake_lsm6dso.c
    src/fake_ssd1306.c
    src/fake_bme280.c
)
```

- [ ] **Step 2: Uncomment the fake DT nodes in the overlay**

In `tests/zephyr/chips/boards/native_sim_native_64.overlay`, replace the commented block inside `i2c0_emul` (lines 30–52) with:

```dts
            fake_lsm6dso: fake_lsm6dso@6a {
                compatible = "alp,fake-lsm6dso";
                reg = <0x6a>;
            };
            fake_ssd1306: fake_ssd1306@3c {
                compatible = "alp,fake-ssd1306";
                reg = <0x3c>;
            };
            fake_bme280: fake_bme280@76 {
                compatible = "alp,fake-bme280";
                reg = <0x76>;
            };
```

(The fake-using ZTESTs in `src/main.c` are gated on `DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso))` — they re-arm automatically.)

- [ ] **Step 3: Build + run the chips suite**

```
wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
  export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
  export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
  export ZEPHYR_TOOLCHAIN_VARIANT=host && \
  python3 zephyr/scripts/twister \
    --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/zephyr/chips \
    -p native_sim/native/64 -O /tmp/tw-chips'
```

Expected: PASS (link clean, existing fake-backed lsm6dso/ssd1306/bme280 tests now execute). Never pipe twister through `| tail`; read `/tmp/tw-chips/twister.log` for the summary line.

**If the `__device_dts_ord_<N>` link error reproduces under 4.4:** add a paired no-op device per fake at the bottom of each `fake_<chip>.c` (after the `EMUL_DT_INST_DEFINE` foreach):

```c
#define FAKE_DEV_DEFINE(n) \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_DEV_DEFINE)
```

and rebuild. If it STILL fails, stop and report — do not ship register tests that compile to nothing.

- [ ] **Step 4: Commit**

```
git add tests/zephyr/chips/CMakeLists.txt tests/zephyr/chips/boards/native_sim_native_64.overlay
git commit -q -m "test(chips): re-enable the i2c-emul fakes -- the EMUL_DT_INST_DEFINE link gap was a Zephyr 3.7 artifact, gone under the v4.4.0 pin"
```

(Bare `git commit -q -m`, staged in a separate prior call — the pre-bash hook false-positives on chained/amended commits.)

---

### Task 2: Shared 8-bit register-file emulator core

Seven new fakes follow one shape: a 256-byte register file, pointer-write/auto-increment-read I2C semantics, an optional per-chip write hook (for W1C registers), and a write log so tests can assert ordering (the DA9292 VSTEP trap needs it).

**Files:**
- Create: `tests/zephyr/chips/src/fake_reg8.h`

- [ ] **Step 1: Write the core header**

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared 8-bit register-file core for the chip-driver i2c-emul
 * fakes.  Models the canonical register-pointer protocol:
 *   write  [reg, d0, d1, ...]  -- set pointer, store bytes (auto-inc)
 *   write  [reg] + read N      -- set pointer, read N bytes (auto-inc)
 * A per-chip write hook overrides plain stores (e.g. RWC1 event
 * registers).  Every data-byte write is appended to a small log so
 * tests can assert WRITE ORDER, not just final state.
 */
#ifndef ALP_TESTS_FAKE_REG8_H
#define ALP_TESTS_FAKE_REG8_H

#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>

#define FAKE_REG8_LOG_DEPTH 32u

struct fake_reg8_wr {
    uint8_t reg;
    uint8_t val;
};

struct fake_reg8 {
    uint8_t regs[256];
    uint8_t ptr;
    void (*write_hook)(struct fake_reg8 *f, uint8_t reg, uint8_t val);
    struct fake_reg8_wr log[FAKE_REG8_LOG_DEPTH];
    uint8_t             log_count; /* saturates at FAKE_REG8_LOG_DEPTH */
};

static inline void fake_reg8_reset(struct fake_reg8 *f)
{
    void (*hook)(struct fake_reg8 *, uint8_t, uint8_t) = f->write_hook;
    memset(f, 0, sizeof(*f));
    f->write_hook = hook;
}

static inline void fake_reg8_store(struct fake_reg8 *f, uint8_t reg, uint8_t val)
{
    if (f->log_count < FAKE_REG8_LOG_DEPTH) {
        f->log[f->log_count].reg = reg;
        f->log[f->log_count].val = val;
        f->log_count++;
    }
    if (f->write_hook != NULL) {
        f->write_hook(f, reg, val);
    } else {
        f->regs[reg] = val;
    }
}

static inline int fake_reg8_transfer(struct fake_reg8 *f, struct i2c_msg *msgs, int num_msgs)
{
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if ((m->flags & I2C_MSG_READ) != 0u) {
            for (uint32_t j = 0; j < m->len; j++) {
                m->buf[j] = f->regs[f->ptr];
                f->ptr++;
            }
        } else {
            if (m->len == 0u) continue;
            f->ptr = m->buf[0];
            for (uint32_t j = 1; j < m->len; j++) {
                fake_reg8_store(f, f->ptr, m->buf[j]);
                f->ptr++;
            }
        }
    }
    return 0;
}

#endif /* ALP_TESTS_FAKE_REG8_H */
```

- [ ] **Step 2: Commit**

```
git add tests/zephyr/chips/src/fake_reg8.h
git commit -q -m "test(chips): shared fake_reg8 register-file core -- pointer protocol, per-chip write hooks, ordered write log"
```

---

### Task 3: DA9292 — fake, register tests, citation cleanup

The driver's guessed STATUS_00 layout was **verified correct** against DA9292 Datasheet Rev 2.2 (R16DS0518EJ0220): Table 14 confirms bit7..0 = S_CH2_OC..S_CH1_PG exactly as the defines stand. Code change = comments only; the real work is locking the behaviour with register tests.

**Files:**
- Create: `tests/zephyr/chips/src/fake_da9292.c`
- Create: `tests/zephyr/chips/dts/bindings/alp,fake-da9292.yaml`
- Modify: `tests/zephyr/chips/src/fakes.h` (declare helpers)
- Modify: `tests/zephyr/chips/boards/native_sim_native_64.overlay` (add node)
- Modify: `tests/zephyr/chips/CMakeLists.txt` (add source)
- Modify: `tests/zephyr/chips/src/main.c` (tests)
- Modify: `chips/da9292/da9292.c:45-55` (comment block)
- Modify: `metadata/chips/da9292.yaml:4-5` (driver_status)

- [ ] **Step 1: Write the DT binding** `tests/zephyr/chips/dts/bindings/alp,fake-da9292.yaml`

```yaml
description: Fake Renesas DA9292 PMIC for i2c-emul chip tests.
compatible: "alp,fake-da9292"
include: i2c-device.yaml
```

- [ ] **Step 2: Write the fake** `tests/zephyr/chips/src/fake_da9292.c`

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake DA9292 PMIC.  Register truth: DA9292 Datasheet Rev 2.2
 * (R16DS0518EJ0220).  Seeds the DA9292-AROVx V2N boot state the
 * driver must cope with: CH2_VSTEP=1 in PMC_CTRL_01 (the 1.5 V OTP
 * trap) and the Table-24 description-table VOUT default 0xA3.
 * PMC_EVENT_00/01 are RWC1 (write-1-to-clear) via the write hook.
 */
#define DT_DRV_COMPAT alp_fake_da9292

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_da9292_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_da9292;

static void da9292_write_hook(struct fake_reg8 *f, uint8_t reg, uint8_t val)
{
    if (reg == 0x02u || reg == 0x03u) {   /* PMC_EVENT_00/01: RWC1 */
        f->regs[reg] &= (uint8_t)~val;
    } else {
        f->regs[reg] = val;
    }
}

static void fake_da9292_seed(struct fake_reg8 *f)
{
    f->regs[0x04] = 0xFF; /* PMC_MASK_00 reset (Table 12)            */
    f->regs[0x05] = 0x07; /* PMC_MASK_01 reset                       */
    f->regs[0x06] = 0x04; /* PMC_CTRL_00: CHSEL=1? no -- 2ch: 0x04   */
    f->regs[0x07] = 0x80; /* PMC_CTRL_01: CH2_VSTEP=1 (AROVx OTP)    */
    f->regs[0x08] = 0x3F; /* PMC_CTRL_02 reset                       */
    f->regs[0x09] = 0xFF; /* PMC_CTRL_03 reset                       */
    f->regs[0x0A] = 0xA3; /* VOUT_CH1_00 = 0.815 V (Table 24)        */
    f->regs[0x0B] = 0xA3;
    f->regs[0x0C] = 0xA3;
    f->regs[0x0D] = 0xA3;
    f->regs[0x19] = 0xEA; /* PMC_DEV_ID reset                        */
    f->regs[0x1A] = 0x10; /* PMC_REV_ID reset                        */
}

uint8_t fake_da9292_get_reg(uint8_t reg)            { return g_da9292->regs[reg]; }
void    fake_da9292_set_reg(uint8_t reg, uint8_t v) { g_da9292->regs[reg] = v; }
uint8_t fake_da9292_log_count(void)                 { return g_da9292->log_count; }
void    fake_da9292_log_at(uint8_t i, uint8_t *reg, uint8_t *val)
{
    *reg = g_da9292->log[i].reg;
    *val = g_da9292->log[i].val;
}
void fake_da9292_reset(void)
{
    fake_reg8_reset(g_da9292);
    fake_da9292_seed(g_da9292);
}

static int fake_da9292_transfer(const struct emul *target, struct i2c_msg *msgs,
                                int num_msgs, int addr)
{
    ARG_UNUSED(addr);
    struct fake_da9292_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_da9292_api = {
    .transfer = fake_da9292_transfer,
};

static int fake_da9292_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_da9292_data *data = target->data;
    data->rf.write_hook = da9292_write_hook;
    fake_reg8_reset(&data->rf);
    fake_da9292_seed(&data->rf);
    g_da9292 = &data->rf;
    return 0;
}

#define FAKE_DA9292_DEFINE(n)                                                  \
    static struct fake_da9292_data fake_da9292_data_##n;                       \
    EMUL_DT_INST_DEFINE(n, fake_da9292_init, &fake_da9292_data_##n, NULL,      \
                        &fake_da9292_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_DA9292_DEFINE)
```

- [ ] **Step 3: Declare helpers in `fakes.h`** (append, matching the existing style)

```c
/* fake_da9292.c -- DA9292 secondary PMIC @ 0x1e. */
uint8_t fake_da9292_get_reg(uint8_t reg);
void    fake_da9292_set_reg(uint8_t reg, uint8_t val);
uint8_t fake_da9292_log_count(void);
void    fake_da9292_log_at(uint8_t i, uint8_t *reg, uint8_t *val);
void    fake_da9292_reset(void);
```

- [ ] **Step 4: Add the overlay node** (inside `i2c0_emul`, after the bme280 node)

```dts
            fake_da9292: fake_da9292@1e {
                compatible = "alp,fake-da9292";
                reg = <0x1e>;
            };
```

- [ ] **Step 5: Add `src/fake_da9292.c` to `target_sources` in CMakeLists.txt**

- [ ] **Step 6: Write the failing tests** (append to `tests/zephyr/chips/src/main.c`, inside the existing `#if DT_NODE_EXISTS` fake section or a new `#if DT_NODE_EXISTS(DT_NODELABEL(fake_da9292))` block)

First check how the existing lsm6dso fake tests obtain their bus handle (main.c lines ~773–1049). If a shared open-bus helper exists, use it under its real name everywhere this plan writes `chips_test_bus()`. If none exists, add this helper once near the fake test section:

```c
/* Shared bus handle for the fake-backed register tests -- bus_id 0
 * resolves to the i2c0_emul controller via the alp-i2c0 DT alias. */
static alp_i2c_t *chips_test_bus(void)
{
    static alp_i2c_t *bus;
    if (bus == NULL) {
        bus = alp_i2c_open(&(alp_i2c_config_t){
            .bus_id     = 0u,
            .bitrate_hz = 400000u,
        });
    }
    return bus;
}
```

```c
#if DT_NODE_EXISTS(DT_NODELABEL(fake_da9292))

ZTEST(alp_chips, test_da9292_fake_probe_reads_ids)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);
    zassert_equal(pmic.dev_id, 0xEA, "PMC_DEV_ID reset value (Table 12 p.35)");
    zassert_equal(pmic.rev_id, 0x10, "PMC_REV_ID reset value");
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_status_decode_matches_table14)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    /* Asymmetric pattern across the CH2-upper/CH1-lower pairs:
     * S_CH2_OC (bit7) | S_CH1_UV (bit2) | S_CH1_PG (bit0) = 0x85. */
    fake_da9292_set_reg(0x00, 0x85);
    fake_da9292_set_reg(0x01, 0x05); /* S_TEMP_WARN | S_VIN_UVLO */

    da9292_status_t st;
    zassert_equal(da9292_get_status(&pmic, &st), ALP_OK);
    zassert_true(st.ch2_oc);
    zassert_false(st.ch1_oc);
    zassert_true(st.ch1_uv);
    zassert_false(st.ch2_uv);
    zassert_true(st.ch1_pg);
    zassert_false(st.ch2_pg);
    zassert_false(st.ch1_ov);
    zassert_false(st.ch2_ov);
    zassert_true(st.temp_warn);
    zassert_true(st.vin_uvlo);
    zassert_false(st.temp_crit);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_events_write1_to_clear)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    fake_da9292_set_reg(0x02, 0x18); /* E_CH1_OV | E_CH2_UV */
    da9292_events_t ev;
    zassert_equal(da9292_read_and_clear_events(&pmic, &ev), ALP_OK);
    zassert_true(ev.e_ch1_ov);
    zassert_true(ev.e_ch2_uv);
    zassert_false(ev.e_ch2_ov);
    /* RWC1: the driver echoed 0x18 back; the hook cleared the bits. */
    zassert_equal(fake_da9292_get_reg(0x02), 0x00);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_voltage_encoding_roundtrip)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    /* 750 mV at VSTEP=0: 0x3C + (750-300)/5 = 0x3C + 90 = 0x96. */
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH2, 750), ALP_OK);
    zassert_equal(fake_da9292_get_reg(0x0C), 0x96, "Table 24 encoding");
    uint16_t mv = 0;
    zassert_equal(da9292_get_voltage_mv(&pmic, DA9292_CH2, &mv), ALP_OK);
    zassert_equal(mv, 750);

    /* Range guards: 0x00..0x3B are reserved bytes; <300 / >1275 mV invalid. */
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH1, 299), ALP_ERR_INVAL);
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH1, 1280), ALP_ERR_INVAL);
    fake_da9292_set_reg(0x0A, 0x3B); /* reserved byte */
    zassert_equal(da9292_get_voltage_mv(&pmic, DA9292_CH1, &mv), ALP_ERR_IO);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_deepx_rail_clears_vstep_before_vout)
{
    /* THE trap: AROVx OTP boots CH2_VSTEP=1; writing the 0.75 V byte
     * (0x96) at VSTEP=1 would put 1.5 V on DEEPX.  Assert the driver
     * clears VSTEP (a CTRL_01 write) BEFORE any VOUT_CH2 write, via
     * the fake's ordered write log. */
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);
    fake_da9292_set_reg(0x00, 0x02); /* pre-seed S_CH2_PG so the poll exits */

    zassert_equal(da9292_v2n_m1_enable_deepx_rail(&pmic, 1000), ALP_OK);

    /* Walk the log: the first CTRL_01 (0x07) write must precede the
     * first VOUT_CH2_00 (0x0C) write, and must have VSTEP (bit7) low. */
    int idx_ctrl = -1, idx_vout = -1;
    for (uint8_t i = 0; i < fake_da9292_log_count(); i++) {
        uint8_t r, v;
        fake_da9292_log_at(i, &r, &v);
        if (r == 0x07 && idx_ctrl < 0) {
            idx_ctrl = i;
            zassert_equal(v & 0x80, 0, "VSTEP must be cleared by the first CTRL_01 write");
        }
        if (r == 0x0C && idx_vout < 0) idx_vout = i;
    }
    zassert_true(idx_ctrl >= 0, "driver never wrote PMC_CTRL_01");
    zassert_true(idx_vout >= 0, "driver never wrote PMC_VOUT_CH2_00");
    zassert_true(idx_ctrl < idx_vout, "VSTEP clear must precede the VOUT write");
    zassert_equal(fake_da9292_get_reg(0x0C), 0x96);
    /* Final CTRL_01: CH2_EN set, VSTEP still clear. */
    zassert_equal(fake_da9292_get_reg(0x07) & 0x82, 0x02);
    da9292_deinit(&pmic);
}

#endif /* fake_da9292 */
```

- [ ] **Step 7: Run — expect FAIL (only if a decode bug exists) or PASS**

Same twister command as Task 1 Step 3. These tests lock verified-correct behaviour; they should PASS against the current driver. A failure here means the driver diverges from the datasheet — investigate before touching anything.

- [ ] **Step 8: Replace the TODO comment block in `chips/da9292/da9292.c` lines 45–55** with:

```c
/* PMC_STATUS_00 bit layout -- VERIFIED against DA9292 Datasheet
 * Rev 2.2 (R16DS0518EJ0220), Table 14 (p.36-37) on 2026-06-06:
 * bits[7:0] = S_CH2_OC, S_CH1_OC, S_CH2_OV, S_CH1_OV,
 *             S_CH2_UV, S_CH1_UV, S_CH2_PG, S_CH1_PG.
 * (Same ordering as PMC_MASK_00, Table 18 -- the historical
 * mirror-the-mask assumption checked out.) */
```

Also update the header `da9292.h`'s verification-status block (if it carries a "bit layout TODO" caveat) to cite Table 14 as verified.

- [ ] **Step 9: Update `metadata/chips/da9292.yaml`** — change:

```yaml
driver_status:    complete      # full register map; PMC_STATUS/EVENT/
                                # CTRL/VOUT encodings verified vs
                                # datasheet Rev 2.2 Tables 12-27 (2026-06-06)
```

- [ ] **Step 10: Re-run the chips suite (PASS) and commit**

```
git add tests/zephyr/chips chips/da9292/da9292.c include/alp/chips/da9292.h metadata/chips/da9292.yaml
git commit -q -m "test(da9292): register-level fake + ordered-write-log tests; STATUS_00 layout verified vs datasheet Rev 2.2 Table 14 -- the mirror-of-MASK assumption was correct, TODO retired"
```

---

### Task 4: ACT8760 — register-map rework (the guessed map was wrong everywhere)

**Files:**
- Modify: `chips/act8760/act8760.c`
- Modify: `include/alp/chips/act8760.h`
- Create: `tests/zephyr/chips/src/fake_act8760.c`
- Create: `tests/zephyr/chips/dts/bindings/alp,fake-act8760.yaml`
- Modify: `tests/zephyr/chips/src/fakes.h`, `boards/native_sim_native_64.overlay`, `CMakeLists.txt`, `src/main.c`
- Modify: `metadata/chips/act8760.yaml`

- [ ] **Step 1: Write the DT binding** `tests/zephyr/chips/dts/bindings/alp,fake-act8760.yaml`

```yaml
description: Fake Qorvo ACT88760 PMIC slave (one node per I2C address; the chip exposes two).
compatible: "alp,fake-act8760"
include: i2c-device.yaml
```

- [ ] **Step 2: Write the fake** `tests/zephyr/chips/src/fake_act8760.c` — TWO instances (ADD1 @0x25, ADD2 @0x26), keyed by DT reg address:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake ACT88760 PMIC.  The real chip is TWO I2C slaves: ADD1 (0x25
 * on CMI 120.E1) carries MSTR + GPIO + Buck1..6 tiles; ADD2 (0x26)
 * carries Buck7 + LDO1..6.  One DT node per slave, same compatible;
 * instances self-sort by their DT reg address at init.
 * Register truth: AA82BZ_RegisterMap_Users_Rev1P1 workbook +
 * ACT88760 Datasheet Rev C (verified 2026-06-06).
 */
#define DT_DRV_COMPAT alp_fake_act8760

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/devicetree.h>

#include "fake_reg8.h"
#include "fakes.h"

struct fake_act8760_cfg {
    uint8_t addr_7bit;
};

struct fake_act8760_data {
    struct fake_reg8 rf;
};

static struct fake_reg8 *g_act8760_add1; /* 0x25 */
static struct fake_reg8 *g_act8760_add2; /* 0x26 */

static struct fake_reg8 *slave_for(uint8_t addr_7bit)
{
    return (addr_7bit == 0x26u) ? g_act8760_add2 : g_act8760_add1;
}

uint8_t fake_act8760_get_reg(uint8_t addr_7bit, uint8_t reg)
{
    return slave_for(addr_7bit)->regs[reg];
}
void fake_act8760_set_reg(uint8_t addr_7bit, uint8_t reg, uint8_t val)
{
    slave_for(addr_7bit)->regs[reg] = val;
}
void fake_act8760_reset(void)
{
    fake_reg8_reset(g_act8760_add1);
    fake_reg8_reset(g_act8760_add2);
}

static int fake_act8760_transfer(const struct emul *target, struct i2c_msg *msgs,
                                 int num_msgs, int addr)
{
    ARG_UNUSED(addr);
    struct fake_act8760_data *data = target->data;
    return fake_reg8_transfer(&data->rf, msgs, num_msgs);
}

static const struct i2c_emul_api fake_act8760_api = {
    .transfer = fake_act8760_transfer,
};

static int fake_act8760_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    struct fake_act8760_data      *data = target->data;
    const struct fake_act8760_cfg *cfg  = target->cfg;
    fake_reg8_reset(&data->rf);
    if (cfg->addr_7bit == 0x26u) {
        g_act8760_add2 = &data->rf;
    } else {
        g_act8760_add1 = &data->rf;
    }
    return 0;
}

#define FAKE_ACT8760_DEFINE(n)                                                  \
    static struct fake_act8760_data fake_act8760_data_##n;                      \
    static const struct fake_act8760_cfg fake_act8760_cfg_##n = {               \
        .addr_7bit = DT_INST_REG_ADDR(n),                                       \
    };                                                                          \
    EMUL_DT_INST_DEFINE(n, fake_act8760_init, &fake_act8760_data_##n,           \
                        &fake_act8760_cfg_##n, &fake_act8760_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_ACT8760_DEFINE)
```

- [ ] **Step 3: fakes.h, overlay, CMake**

`fakes.h` (append):

```c
/* fake_act8760.c -- ACT88760 primary PMIC, two slaves (0x25 + 0x26). */
uint8_t fake_act8760_get_reg(uint8_t addr_7bit, uint8_t reg);
void    fake_act8760_set_reg(uint8_t addr_7bit, uint8_t reg, uint8_t val);
void    fake_act8760_reset(void);
```

Overlay (inside `i2c0_emul`):

```dts
            fake_act8760_add1: fake_act8760@25 {
                compatible = "alp,fake-act8760";
                reg = <0x25>;
            };
            fake_act8760_add2: fake_act8760@26 {
                compatible = "alp,fake-act8760";
                reg = <0x26>;
            };
```

CMakeLists: add `src/fake_act8760.c`.

- [ ] **Step 4: Write the FAILING tests** (append to main.c, gated on `DT_NODE_EXISTS(DT_NODELABEL(fake_act8760_add1))`)

```c
#if DT_NODE_EXISTS(DT_NODELABEL(fake_act8760_add1))

ZTEST(alp_chips, test_act8760_probe_both_slaves)
{
    fake_act8760_reset();
    alp_i2c_t *bus = chips_test_bus();
    act8760_t  pmic;
    zassert_equal(act8760_init(&pmic, bus), ALP_OK);
    act8760_deinit(&pmic);
}

ZTEST(alp_chips, test_act8760_vset_offsets_per_verified_map)
{
    /* Spot-check the three architecturally distinct cases:
     * Buck1 on ADD1, Buck7 on ADD2 (NOT with Buck1-6!), LDO3 as the
     * second LDO of the LDO53 dual tile. */
    fake_act8760_reset();
    alp_i2c_t *bus = chips_test_bus();
    act8760_t  pmic;
    zassert_equal(act8760_init(&pmic, bus), ALP_OK);

    /* Buck1: ADD1 reg 0x42.  Seed EN_OutPD (bit7) high; the write
     * must preserve it (read-modify-write). */
    fake_act8760_set_reg(0x25, 0x42, 0x80);
    zassert_equal(act8760_rail_set_vset(&pmic, ACT8760_RAIL_BUCK1, 60), ALP_OK);
    zassert_equal(fake_act8760_get_reg(0x25, 0x42), 0x80 | 60,
                  "VSET write must land at ADD1 0x42 and preserve bit7");
    uint8_t v = 0;
    zassert_equal(act8760_rail_get_vset(&pmic, ACT8760_RAIL_BUCK1, &v), ALP_OK);
    zassert_equal(v, 60, "get must mask off the control bit");

    /* Buck7: ADD2 reg 0x02 -- the old guessed table had it at the
     * wrong offset; this is the cross-slave regression lock. */
    zassert_equal(act8760_rail_set_vset(&pmic, ACT8760_RAIL_BUCK7, 0x30), ALP_OK);
    zassert_equal(fake_act8760_get_reg(0x26, 0x02), 0x30);

    /* LDO3: ADD2 reg 0x47 (second LDO of the LDO53 tile); 6-bit
     * field; RANGE bit (bit7) must survive a write. */
    fake_act8760_set_reg(0x26, 0x47, 0x80);
    zassert_equal(act8760_rail_set_vset(&pmic, ACT8760_RAIL_LDO3, 0x2A), ALP_OK);
    zassert_equal(fake_act8760_get_reg(0x26, 0x47), 0x80 | 0x2A);
    zassert_equal(act8760_rail_get_vset(&pmic, ACT8760_RAIL_LDO3, &v), ALP_OK);
    zassert_equal(v, 0x2A);

    /* LDO width guard: 7-bit values are invalid for 6-bit LDO fields. */
    zassert_equal(act8760_rail_set_vset(&pmic, ACT8760_RAIL_LDO1, 0x40), ALP_ERR_INVAL);
    act8760_deinit(&pmic);
}

ZTEST(alp_chips, test_act8760_status_decode_matches_mstr_sheet)
{
    fake_act8760_reset();
    alp_i2c_t *bus = chips_test_bus();
    act8760_t  pmic;
    zassert_equal(act8760_init(&pmic, bus), ALP_OK);

    /* TWARN (bit5) | VSYSWARN (bit1) = 0x22 -- under the OLD wrong
     * decode this read as SYSDAT|nothing; under the verified map it
     * is thermal_warning + vsys_warning. */
    fake_act8760_set_reg(0x25, 0x00, 0x22);
    act8760_status_t st;
    zassert_equal(act8760_get_status(&pmic, &st), ALP_OK);
    zassert_true(st.thermal_warning);
    zassert_true(st.vsys_warning);
    zassert_false(st.vsys_stat);
    zassert_false(st.vin_pok_ov);
    zassert_false(st.rom_stat);
    zassert_false(st.wd_alert);
    zassert_false(st.pb_assert);
    zassert_false(st.pb_deassert);
    zassert_equal(st.raw, 0x22);
    act8760_deinit(&pmic);
}

#endif /* fake_act8760 */
```

- [ ] **Step 5: Run — expect FAIL** (`rail_set_vset` returns `ALP_ERR_NOSUPPORT`; `act8760_status_t` lacks the new fields → compile error first). That compile error is the TDD signal to proceed.

- [ ] **Step 6: Rework the header** `include/alp/chips/act8760.h`:

6a. Replace the `[PAPER-CORRECT-STUB]` verification paragraph (lines 15–24) with:

```
 * @par Verification status: [REGISTER-MAP-VERIFIED 2026-06-06]
 *   Per-rail VSET0 offsets, the two-slave address model, and the
 *   system-status bit map were extracted from the AA82BZ register-map
 *   workbook (AA82BZ_RegisterMap_Users_Rev1P1) and the ACT88760
 *   Datasheet Rev C, then independently re-derived cell-by-cell.
 *   Still [UNTESTED] on silicon -- see the HiL caveat above.
```

6b. Replace the "I2C slave addressing" paragraph (lines 43–53): the chip presents **two independent slave addresses** (not register pages): ADD1 (selected by MSTR 0x16[7:6]) carries MSTR + GPIO + Buck1–6; ADD2 (MSTR 0x23[4:3]) carries Buck7 + LDO1–6. All four documented CMI pairs are adjacent (0x25/0x26, 0x27/0x28, 0x67/0x68, 0x6B/0x6C), so `act8760_init_at`'s `page0 + 1` derivation holds for every variant. Keep `ACT8760_PAGE_SYSTEM`/`ACT8760_PAGE_AUX` enum names; update their doc comments to `/**< ADD1: MSTR + Buck1..Buck6 + GPIOs (0x25 on CMI 120.E1). */` and `/**< ADD2: Buck7 + LDO1..LDO6 (0x26 on CMI 120.E1). */`.

6c. Replace the "Per-rail voltage selection" paragraph (lines 55–64) — the mapping IS closed-form now:

```
 * @par Per-rail voltage selection
 * Bucks: 7-bit VSET, VOUT = 500 mV + VSET x 5 mV (Output-Low range)
 * or x 25 mV (Output-High); the active range is the rail's Vout_Range
 * bit (Buck5/6 are Output-High only).  LDO1/2: 6-bit VSET, 12.5 mV
 * step, 0.5-1.2875 V (RANGE=0) or 1.2-1.9875 V (RANGE=1).  LDO3-6:
 * 0.5-1.2875 V or 1-4.15 V.  This driver exposes the raw VSET value;
 * mV conversion needs the rail's range bit, which raw `read_reg` can
 * fetch -- a typed mV helper lands when a consumer needs one (YAGNI).
 * Buck1/2/7 additionally have VSET2/VSET3 DVS slots bank-aliased onto
 * the VSET0/1 addresses via MSTR 0x2C bit0 (BAND_SEL) -- out of this
 * driver's scope.
```

6d. Replace `act8760_status_t` (lines 147–155) with:

```c
/** Decoded system status (register 0x00 on the ADD1 slave).  Bit map
 *  per the AA82BZ MSTR sheet (verified 2026-06-06).  VSYSSTAT and
 *  VSYSWARN latch on the VIN falling edge and clear on read; per-
 *  regulator POK/OV/ILIM flags live in each tile's offset-0 register
 *  (reachable via act8760_read_reg), NOT in this byte. */
typedef struct {
    bool rom_stat;        /**< bit7: ROM/CMI configuration-load status. */
    bool wd_alert;        /**< bit6: watchdog timer alert. */
    bool thermal_warning; /**< bit5 TWARN: junction at warning threshold. */
    bool vsys_stat;       /**< bit4: AVIN below VSYS_MON (latched). */
    bool vin_pok_ov;      /**< bit3: VIN power-OK / over-voltage flag. */
    bool pb_assert;       /**< bit2: push-button assert event. */
    bool vsys_warning;    /**< bit1: AVIN below VSYS_WARN (latched). */
    bool pb_deassert;     /**< bit0: push-button de-assert event. */
    uint8_t raw;          /**< Untouched register byte for diagnostics. */
} act8760_status_t;
```

6e. In the `act8760_rail_get_vset`/`set_vset` doc comments, delete the "NOT a single closed-form formula" sentence (now documented in 6c) and add to `set_vset`: `@note Read-modify-write: bits outside the VSET field (EN_OutPD / IPD_SET on bucks, RANGE on LDOs) are preserved.`

- [ ] **Step 7: Rework the driver** `chips/act8760/act8760.c`:

7a. Replace the register defines (lines 16–33) with:

```c
/* MSTR-tile system registers on the ADD1 slave.  Source of truth:
 * AA82BZ_RegisterMap_Users_Rev1P1 workbook, sheet MSTR (per-bit map),
 * corroborated by ACT88760 Datasheet Rev C -- extracted + adversarially
 * re-derived 2026-06-06.  Register 0x00 is the system status byte;
 * 0x01 carries the matching interrupt masks (TMSK = bit 5). */
#define ACT8760_REG_STATUS 0x00u
#define ACT8760_REG_TMASK  0x01u

/* Register 0x00 bit map, MSB->LSB (MSTR sheet, row 0x00):
 *   ROM_STAT | WD_TIMER_ALERT | TWARN | VSYSSTAT | VIN_POK_OV |
 *   PBASTAT | VSYSWARN | PBDSTAT
 * VSYSSTAT / VSYSWARN latch on the VIN falling edge, clear on read.
 * NOTE: SYSDAT (the raw VSYSMON sample) is register 0x02 bit 4, and
 * per-regulator ILIM / OV / POK flags live in each tile's offset-0
 * register -- neither is in this byte. */
#define ACT8760_STATUS_ROM_STAT   0x80u
#define ACT8760_STATUS_WD_ALERT   0x40u
#define ACT8760_STATUS_TWARN      0x20u
#define ACT8760_STATUS_VSYS_STAT  0x10u
#define ACT8760_STATUS_VIN_POK_OV 0x08u
#define ACT8760_STATUS_PBA_STAT   0x04u
#define ACT8760_STATUS_VSYS_WARN  0x02u
#define ACT8760_STATUS_PBD_STAT   0x01u
```

(`ACT8760_REG_GPIO_STAT_LO` and `ACT8760_REG_OV_UV_CFG` were unverified and unused — delete them.)

7b. Replace `rail_table` + its comment (lines 35–69) with:

```c
/* Per-rail VSET0 location.  Each regulator is a 0x20-wide register
 * tile: Buck1..6 at bases 0x40/0x60/0x80/0xA0/0xC0/0xE0 on ADD1;
 * Buck7 at base 0x00 on ADD2; the LDOs pair up in dual tiles on ADD2
 * (LDO12 @0x20, LDO53 @0x40, LDO64 @0x60 -- first LDO at +1, second
 * at +7, hence the LDO5/0x41-before-LDO3/0x47 ordering).  VSET0 sits
 * at buck-tile offset +2 / LDO slot offsets +1 / +7.
 * Bit 7 of every buck VSET byte is a live control bit (EN_OutPD or
 * IPD_SET) and the LDO bytes carry the RANGE bit above the 6-bit
 * field -- the accessors below mask reads and read-modify-write
 * writes accordingly.
 * Source: AA82BZ_RegisterMap_Users_Rev1P1, per-tile sheets (verified
 * cell-by-cell 2026-06-06; tile bases independently corroborated by
 * the MSTR 0x06 INTADR decode table). */
struct rail_loc {
    act8760_page_t page;
    uint8_t        vset0_reg; /* byte address of the VSET0 register */
    uint8_t        vset_mask; /* 0x7F (buck, 7-bit) or 0x3F (LDO, 6-bit) */
};

static const struct rail_loc rail_table[ACT8760_RAIL_COUNT] = {
    [ACT8760_RAIL_BUCK1] = {ACT8760_PAGE_SYSTEM, 0x42, 0x7F},
    [ACT8760_RAIL_BUCK2] = {ACT8760_PAGE_SYSTEM, 0x62, 0x7F},
    [ACT8760_RAIL_BUCK3] = {ACT8760_PAGE_SYSTEM, 0x82, 0x7F},
    [ACT8760_RAIL_BUCK4] = {ACT8760_PAGE_SYSTEM, 0xA2, 0x7F},
    [ACT8760_RAIL_BUCK5] = {ACT8760_PAGE_SYSTEM, 0xC2, 0x7F},
    [ACT8760_RAIL_BUCK6] = {ACT8760_PAGE_SYSTEM, 0xE2, 0x7F},
    [ACT8760_RAIL_BUCK7] = {ACT8760_PAGE_AUX,    0x02, 0x7F},
    [ACT8760_RAIL_LDO1]  = {ACT8760_PAGE_AUX,    0x21, 0x3F},
    [ACT8760_RAIL_LDO2]  = {ACT8760_PAGE_AUX,    0x27, 0x3F},
    [ACT8760_RAIL_LDO3]  = {ACT8760_PAGE_AUX,    0x47, 0x3F},
    [ACT8760_RAIL_LDO4]  = {ACT8760_PAGE_AUX,    0x67, 0x3F},
    [ACT8760_RAIL_LDO5]  = {ACT8760_PAGE_AUX,    0x41, 0x3F},
    [ACT8760_RAIL_LDO6]  = {ACT8760_PAGE_AUX,    0x61, 0x3F},
};
```

7c. Replace the `act8760_get_status` decode body (lines 129–134) with:

```c
    out->raw             = reg;
    out->rom_stat        = (reg & ACT8760_STATUS_ROM_STAT) != 0;
    out->wd_alert        = (reg & ACT8760_STATUS_WD_ALERT) != 0;
    out->thermal_warning = (reg & ACT8760_STATUS_TWARN) != 0;
    out->vsys_stat       = (reg & ACT8760_STATUS_VSYS_STAT) != 0;
    out->vin_pok_ov      = (reg & ACT8760_STATUS_VIN_POK_OV) != 0;
    out->pb_assert       = (reg & ACT8760_STATUS_PBA_STAT) != 0;
    out->vsys_warning    = (reg & ACT8760_STATUS_VSYS_WARN) != 0;
    out->pb_deassert     = (reg & ACT8760_STATUS_PBD_STAT) != 0;
```

7d. Replace both VSET accessors (lines 155–178) with the real implementations:

```c
alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t *vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (vset_raw == NULL) return ALP_ERR_INVAL;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

    const struct rail_loc *loc = &rail_table[rail];
    uint8_t                reg = 0;
    alp_status_t           s   = reg_read(ctx, loc->page, loc->vset0_reg, &reg);
    if (s != ALP_OK) return s;
    *vset_raw = reg & loc->vset_mask;
    return ALP_OK;
}

alp_status_t act8760_rail_set_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;
    const struct rail_loc *loc = &rail_table[rail];
    if ((vset_raw & ~loc->vset_mask) != 0) return ALP_ERR_INVAL;

    /* Read-modify-write: bit 7 (EN_OutPD / IPD_SET on bucks, RANGE
     * on LDO3..6) and bit 6 (RANGE on LDO1/2) are live configuration
     * bits sharing the VSET byte -- clobbering them would change the
     * rail's range or pull-down behaviour. */
    uint8_t      reg = 0;
    alp_status_t s   = reg_read(ctx, loc->page, loc->vset0_reg, &reg);
    if (s != ALP_OK) return s;
    reg = (uint8_t)((reg & ~loc->vset_mask) | vset_raw);
    return reg_write(ctx, loc->page, loc->vset0_reg, reg);
}
```

7e. Fix the file-top provenance comment (lines 5–8): name both sources ("AA82BZ_RegisterMap_Users_Rev1P1 workbook + ACT88760 Datasheet Rev C"), drop the "lines 3096-3097 of the extracted register description" reference. In `act8760_init_at`, update the probe comment: page-1 read targets ADD2 register 0x00 (the Buck7 tile's status byte) — a pure ACK check.

7f. Check consumers compile: `grep -rn "act8760_status_t\|sys_warning\|sys_data\|ilim_warning\|fault_pending" src/ examples/ tests/ firmware/` — as of plan-writing the only consumer is `tests/zephyr/chips/src/main.c:1577-1583` (declaration + uninitialised-guard calls, unaffected). If new consumers appeared since, update their field accesses to the new names.

- [ ] **Step 8: Run the chips suite — expect PASS** (same twister command). Then run clang-format on both touched C files before committing (the enum/define alignment trap): `clang-format-14 -style=file -i chips/act8760/act8760.c include/alp/chips/act8760.h` (via WSL).

- [ ] **Step 9: Update `metadata/chips/act8760.yaml`**:

```yaml
driver_status:    partial       # probe + status + VSET0 accessors live
                                # (register map verified vs the AA82BZ
                                # workbook 2026-06-06); DVS slots VSET1-3,
                                # per-tile fault regs + mV mapping still
                                # raw-R/W-only
```

Also append under `datasheet:`:

```yaml
  register_map:   "AA82BZ_RegisterMap_Users_Rev1P1 Customer Facing.xlsx"
```

- [ ] **Step 10: Commit**

```
git add chips/act8760 include/alp/chips/act8760.h tests/zephyr/chips metadata/chips/act8760.yaml
git commit -q -m "fix(act8760): replace the guessed register map with the verified one -- two-slave model, VSET0 tile offsets (Buck7 on ADD2 with the LDOs), RMW vset accessors un-stubbed, status 0x00 decode corrected (TWARN is bit5, SYSDAT was never in 0x00)"
```

---

### Task 5: Fakes + register tests for rv3028c7, tmp112, clk_5l35023b, tps628640, optiga_trust_m

**Files:**
- Create: `tests/zephyr/chips/src/fake_rv3028c7.c`, `fake_tmp112.c`, `fake_clk_5l35023b.c`, `fake_tps628640.c`, `fake_optiga_trust_m.c`
- Create: `tests/zephyr/chips/dts/bindings/alp,fake-rv3028c7.yaml`, `alp,fake-tmp112.yaml`, `alp,fake-clk-5l35023b.yaml`, `alp,fake-tps628640.yaml`, `alp,fake-optiga-trust-m.yaml`
- Modify: `fakes.h`, overlay, `CMakeLists.txt`, `src/main.c`

- [ ] **Step 1: Bindings** — five 3-line files, same shape as Task 3 Step 1, compatibles `alp,fake-rv3028c7`, `alp,fake-tmp112`, `alp,fake-clk-5l35023b`, `alp,fake-tps628640`, `alp,fake-optiga-trust-m`.

- [ ] **Step 2: Overlay nodes** (inside `i2c0_emul`; addresses = V2N BRD_I2C reality, TMP112 at its datasheet-default 0x48 — the 0x40-vs-0x48 question is a silicon question, not a fake question):

```dts
            fake_rv3028c7: fake_rv3028c7@52 {
                compatible = "alp,fake-rv3028c7";
                reg = <0x52>;
            };
            fake_tmp112: fake_tmp112@48 {
                compatible = "alp,fake-tmp112";
                reg = <0x48>;
            };
            fake_clk_5l35023b: fake_clk_5l35023b@68 {
                compatible = "alp,fake-clk-5l35023b";
                reg = <0x68>;
            };
            fake_tps628640: fake_tps628640@4d {
                compatible = "alp,fake-tps628640";
                reg = <0x4d>;
            };
            fake_optiga: fake_optiga@30 {
                compatible = "alp,fake-optiga-trust-m";
                reg = <0x30>;
            };
```

- [ ] **Step 3: Write the four reg8-based fakes.** Each follows the exact `fake_da9292.c` skeleton (DT_DRV_COMPAT, data struct wrapping `struct fake_reg8`, singleton, transfer, init, `EMUL_DT_INST_DEFINE` foreach). Only seeds/hooks differ — full per-chip deltas:

`fake_rv3028c7.c` (`DT_DRV_COMPAT alp_fake_rv3028c7`; no write hook):

```c
static void fake_rv3028c7_seed(struct fake_reg8 *f)
{
    /* Fresh-from-the-factory state: PORF set (the driver must clear
     * it), CONTROL_2 zero (driver must set the 24H bit), a plausible
     * BCD wall time 2026-06-06 12:34:56, Saturday(7). */
    f->regs[0x00] = 0x56; /* SECONDS, BCD */
    f->regs[0x01] = 0x34; /* MINUTES      */
    f->regs[0x02] = 0x12; /* HOURS        */
    f->regs[0x03] = 0x07; /* WEEKDAY      */
    f->regs[0x04] = 0x06; /* DATE         */
    f->regs[0x05] = 0x06; /* MONTH        */
    f->regs[0x06] = 0x26; /* YEAR (2026)  */
    f->regs[0x0E] = 0x01; /* STATUS: PORF */
}
```

Helpers: `fake_rv3028c7_get_reg/set_reg/reset` (same shape as da9292's, no log accessors).

`fake_clk_5l35023b.c` (`alp_fake_clk_5l35023b`; no hook):

```c
static void fake_clk_5l35023b_seed(struct fake_reg8 *f)
{
    /* Byte 0x00 General Control: I2C_addr strap field bits[6:5] = 0
     * -> the part claims slave 0x68.  Byte 0x01 Dash Code ID: an
     * arbitrary factory stamp the test asserts round-trips.  Byte
     * 0x24 bit7 (I2C_PDB) = 1 -> normal operation. */
    f->regs[0x00] = 0x00;
    f->regs[0x01] = 0x5A;
    f->regs[0x24] = 0x80;
}
```

`fake_tps628640.c` (`alp_fake_tps628640`; no hook):

```c
static void fake_tps628640_seed(struct fake_reg8 *f)
{
    /* VOUT1 = 0x28 -> 400 + 40*5 = 600 mV (the LPDDR4X 0.6 V role
     * this instance plays at 0x4D on the V2N).  STATUS clear. */
    f->regs[TPS628640_REG_VOUT1] = 0x28;
    f->regs[TPS628640_REG_VOUT2] = 0x28;
}
```

(include `"alp/chips/tps628640.h"` for the register macros.)

`fake_optiga_trust_m.c` (`alp_fake_optiga_trust_m`; no hook):

```c
static void fake_optiga_seed(struct fake_reg8 *f)
{
    /* I2C_STATE register (0x82) reads 4 bytes; an idle Trust M
     * reports BUSY=0 / RESP_RDY=0 in the first byte.  All zeros is
     * the canonical idle answer the driver's probe accepts. */
    f->regs[0x82] = 0x00;
    f->regs[0x83] = 0x00;
    f->regs[0x84] = 0x00;
    f->regs[0x85] = 0x00;
}
```

- [ ] **Step 4: Write `fake_tmp112.c`** — NOT reg8-based: the TMP112 has four 16-bit registers, MSB-first on the wire:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake TI TMP112.  Four 16-bit registers, big-endian on the wire:
 * reads return [MSB, LSB] of the pointed register; writes take
 * [reg, MSB, LSB].  Seeds the datasheet power-on CONF (0x60A0 --
 * R1:R0 read 11, which the driver's probe fingerprints).
 */
#define DT_DRV_COMPAT alp_fake_tmp112

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>

#include "fakes.h"

struct fake_tmp112_data {
    uint16_t regs[4];
    uint8_t  ptr;
};

static struct fake_tmp112_data *g_tmp112;

uint16_t fake_tmp112_get_reg(uint8_t reg)              { return g_tmp112->regs[reg & 0x3]; }
void     fake_tmp112_set_reg(uint8_t reg, uint16_t v)  { g_tmp112->regs[reg & 0x3] = v; }
void     fake_tmp112_reset(void)
{
    memset(g_tmp112->regs, 0, sizeof(g_tmp112->regs));
    g_tmp112->regs[1] = 0x60A0; /* CONF power-on default */
    g_tmp112->ptr     = 0;
}

static int fake_tmp112_transfer(const struct emul *target, struct i2c_msg *msgs,
                                int num_msgs, int addr)
{
    ARG_UNUSED(addr);
    struct fake_tmp112_data *d = target->data;
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if ((m->flags & I2C_MSG_READ) != 0u) {
            uint16_t v = d->regs[d->ptr & 0x3];
            if (m->len >= 1) m->buf[0] = (uint8_t)(v >> 8);
            if (m->len >= 2) m->buf[1] = (uint8_t)(v & 0xFF);
        } else {
            if (m->len >= 1) d->ptr = m->buf[0];
            if (m->len >= 3) {
                d->regs[d->ptr & 0x3] =
                    (uint16_t)(((uint16_t)m->buf[1] << 8) | m->buf[2]);
            }
        }
    }
    return 0;
}

static const struct i2c_emul_api fake_tmp112_api = {
    .transfer = fake_tmp112_transfer,
};

static int fake_tmp112_init(const struct emul *target, const struct device *parent)
{
    ARG_UNUSED(parent);
    g_tmp112 = target->data;
    fake_tmp112_reset();
    return 0;
}

#define FAKE_TMP112_DEFINE(n)                                              \
    static struct fake_tmp112_data fake_tmp112_data_##n;                   \
    EMUL_DT_INST_DEFINE(n, fake_tmp112_init, &fake_tmp112_data_##n, NULL,  \
                        &fake_tmp112_api, NULL);
DT_INST_FOREACH_STATUS_OKAY(FAKE_TMP112_DEFINE)
```

- [ ] **Step 5: fakes.h declarations** (append):

```c
/* fake_rv3028c7.c -- RV-3028-C7 RTC @ 0x52. */
uint8_t  fake_rv3028c7_get_reg(uint8_t reg);
void     fake_rv3028c7_set_reg(uint8_t reg, uint8_t val);
void     fake_rv3028c7_reset(void);
/* fake_tmp112.c -- TMP112 temp sensor @ 0x48 (16-bit registers). */
uint16_t fake_tmp112_get_reg(uint8_t reg);
void     fake_tmp112_set_reg(uint8_t reg, uint16_t val);
void     fake_tmp112_reset(void);
/* fake_clk_5l35023b.c -- 5L35023B clock generator @ 0x68. */
uint8_t  fake_clk_5l35023b_get_reg(uint8_t reg);
void     fake_clk_5l35023b_set_reg(uint8_t reg, uint8_t val);
void     fake_clk_5l35023b_reset(void);
/* fake_tps628640.c -- TPS628640 buck @ 0x4d. */
uint8_t  fake_tps628640_get_reg(uint8_t reg);
void     fake_tps628640_set_reg(uint8_t reg, uint8_t val);
void     fake_tps628640_reset(void);
/* fake_optiga_trust_m.c -- OPTIGA Trust M @ 0x30. */
uint8_t  fake_optiga_get_reg(uint8_t reg);
void     fake_optiga_set_reg(uint8_t reg, uint8_t val);
void     fake_optiga_reset(void);
```

- [ ] **Step 6: CMakeLists** — add the five new sources to `target_sources`.

- [ ] **Step 7: Write the tests** (append to main.c, each block gated on its fake's `DT_NODE_EXISTS`):

```c
#if DT_NODE_EXISTS(DT_NODELABEL(fake_rv3028c7))

ZTEST(alp_chips, test_rv3028c7_init_clears_porf_and_forces_24h)
{
    fake_rv3028c7_reset();
    alp_i2c_t  *bus = chips_test_bus();
    rv3028c7_t rtc;
    zassert_equal(rv3028c7_init(&rtc, bus), ALP_OK);
    zassert_equal(fake_rv3028c7_get_reg(0x0E) & 0x01, 0, "PORF must be cleared");
    zassert_true((fake_rv3028c7_get_reg(0x10) & 0x40) != 0, "CTRL2 24H must be set");
    rv3028c7_deinit(&rtc);
}

ZTEST(alp_chips, test_rv3028c7_time_bcd_roundtrip)
{
    fake_rv3028c7_reset();
    alp_i2c_t  *bus = chips_test_bus();
    rv3028c7_t rtc;
    zassert_equal(rv3028c7_init(&rtc, bus), ALP_OK);

    /* Seeded fake time is 2026-06-06 12:34:56 in BCD. */
    rv3028c7_time_t t;
    zassert_equal(rv3028c7_get_time(&rtc, &t), ALP_OK);
    zassert_equal(t.year, 2026);
    zassert_equal(t.month, 6);
    zassert_equal(t.day, 6);
    zassert_equal(t.hour, 12);
    zassert_equal(t.minute, 34);
    zassert_equal(t.second, 56);

    /* Write a tricky BCD value (59 -> 0x59, 23 -> 0x23) and check
     * the raw register encoding, not just the round-trip. */
    const rv3028c7_time_t set = {
        .second = 59, .minute = 8, .hour = 23,
        .weekday = 1, .day = 31, .month = 12, .year = 2030,
    };
    zassert_equal(rv3028c7_set_time(&rtc, &set), ALP_OK);
    zassert_equal(fake_rv3028c7_get_reg(0x00), 0x59);
    zassert_equal(fake_rv3028c7_get_reg(0x01), 0x08);
    zassert_equal(fake_rv3028c7_get_reg(0x02), 0x23);
    zassert_equal(fake_rv3028c7_get_reg(0x04), 0x31);
    zassert_equal(fake_rv3028c7_get_reg(0x05), 0x12);
    zassert_equal(fake_rv3028c7_get_reg(0x06), 0x30);
    rv3028c7_deinit(&rtc);
}

ZTEST(alp_chips, test_rv3028c7_alarm_match_mask_encoding)
{
    /* The RV-3028's alarm registers carry an AE_x bit (0x80): SET
     * means "don't care", CLEAR means "participate in the match".
     * Verify the driver encodes the match struct that way and BCDs
     * the field values.  (Confirm the AE polarity against
     * chips/rv3028c7/rv3028c7.c RV3028_ALARM_AE before trusting a
     * failure -- the datasheet is the tiebreaker.) */
    fake_rv3028c7_reset();
    alp_i2c_t  *bus = chips_test_bus();
    rv3028c7_t rtc;
    zassert_equal(rv3028c7_init(&rtc, bus), ALP_OK);

    const rv3028c7_time_t when = {
        .second = 0, .minute = 30, .hour = 7,
        .weekday = 2, .day = 15, .month = 1, .year = 2026,
    };
    const rv3028c7_alarm_match_t match = {
        .match_minute         = true,
        .match_hour           = true,
        .match_day_or_weekday = false,
        .use_weekday          = false,
    };
    zassert_equal(rv3028c7_set_alarm(&rtc, &when, &match), ALP_OK);
    zassert_equal(fake_rv3028c7_get_reg(0x07), 0x30, "minute: BCD 30, AE clear");
    zassert_equal(fake_rv3028c7_get_reg(0x08), 0x07, "hour: BCD 7, AE clear");
    zassert_equal(fake_rv3028c7_get_reg(0x09) & 0x80, 0x80,
                  "day/weekday: AE set = excluded from the match");
    rv3028c7_deinit(&rtc);
}

#endif /* fake_rv3028c7 */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_tmp112))

ZTEST(alp_chips, test_tmp112_probe_fingerprints_conf)
{
    fake_tmp112_reset();
    alp_i2c_t *bus = chips_test_bus();
    tmp112_t   sens;
    zassert_equal(tmp112_init(&sens, bus, TMP112_I2C_ADDR_GND), ALP_OK);
    tmp112_deinit(&sens);

    /* A CONF without R1:R0 = 11 is not a TMP112 -- probe must reject. */
    fake_tmp112_set_reg(1, 0x0000);
    zassert_equal(tmp112_init(&sens, bus, TMP112_I2C_ADDR_GND), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_tmp112_temperature_sign_extension)
{
    fake_tmp112_reset();
    alp_i2c_t *bus = chips_test_bus();
    tmp112_t   sens;
    zassert_equal(tmp112_init(&sens, bus, TMP112_I2C_ADDR_GND), ALP_OK);

    int32_t mc = 0;
    /* +25.000 C: raw12 = 400 -> reg = 400<<4 = 0x1900. */
    fake_tmp112_set_reg(0, 0x1900);
    zassert_equal(tmp112_read_temp_milli_c(&sens, &mc), ALP_OK);
    zassert_equal(mc, 25000);
    /* -25.000 C: raw12 = -400 -> reg = (int16)(-400<<4) = 0xE700. */
    fake_tmp112_set_reg(0, 0xE700);
    zassert_equal(tmp112_read_temp_milli_c(&sens, &mc), ALP_OK);
    zassert_equal(mc, -25000);
    /* One LSB = 62 mC after integer truncation (625/10). */
    fake_tmp112_set_reg(0, 0x0010);
    zassert_equal(tmp112_read_temp_milli_c(&sens, &mc), ALP_OK);
    zassert_equal(mc, 62);

    /* Extended (13-bit) mode: -25.000 C -> raw13 = -400 -> -400<<3 = 0xF380. */
    zassert_equal(tmp112_set_extended_mode(&sens, true), ALP_OK);
    fake_tmp112_set_reg(0, 0xF380);
    zassert_equal(tmp112_read_temp_milli_c(&sens, &mc), ALP_OK);
    zassert_equal(mc, -25000);
    tmp112_deinit(&sens);
}

#endif /* fake_tmp112 */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_clk_5l35023b))

ZTEST(alp_chips, test_clk_5l35023b_probe_validates_strap)
{
    fake_clk_5l35023b_reset();
    alp_i2c_t       *bus = chips_test_bus();
    clk_5l35023b_t  clk;
    zassert_equal(clk_5l35023b_init(&clk, bus, 0x68), ALP_OK);
    uint8_t dash = 0;
    zassert_equal(clk_5l35023b_read_dashcode_id(&clk, &dash), ALP_OK);
    zassert_equal(dash, 0x5A, "dash code must round-trip from byte 0x01");
    clk_5l35023b_deinit(&clk);

    /* Strap says 0x69 but we asked at 0x68 -> mis-strap rejection. */
    fake_clk_5l35023b_set_reg(0x00, 0x20); /* strap field bits[6:5] = 1 */
    zassert_equal(clk_5l35023b_init(&clk, bus, 0x68), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_clk_5l35023b_power_down_toggles_pdb)
{
    fake_clk_5l35023b_reset();
    alp_i2c_t       *bus = chips_test_bus();
    clk_5l35023b_t  clk;
    zassert_equal(clk_5l35023b_init(&clk, bus, 0x68), ALP_OK);
    zassert_equal(clk_5l35023b_set_power_down(&clk, true), ALP_OK);
    zassert_equal(fake_clk_5l35023b_get_reg(0x24) & 0x80, 0x00, "PDB low = powered down");
    zassert_equal(clk_5l35023b_set_power_down(&clk, false), ALP_OK);
    zassert_equal(fake_clk_5l35023b_get_reg(0x24) & 0x80, 0x80);
    clk_5l35023b_deinit(&clk);
}

#endif /* fake_clk_5l35023b */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_tps628640))

ZTEST(alp_chips, test_tps628640_voltage_roundtrip_and_bounds)
{
    fake_tps628640_reset();
    alp_i2c_t    *bus = chips_test_bus();
    tps628640_t  buck;
    zassert_equal(tps628640_init(&buck, bus, 0x4D, 600), ALP_OK);

    uint16_t mv = 0;
    zassert_equal(tps628640_get_voltage_mv(&buck, &mv), ALP_OK);
    zassert_equal(mv, 600, "seeded 0x28 = 600 mV");

    /* (1100-400)/5 = 140 = 0x8C. */
    zassert_equal(tps628640_set_voltage_mv(&buck, 1100), ALP_OK);
    zassert_equal(fake_tps628640_get_reg(TPS628640_REG_VOUT1), 0x8C);
    zassert_equal(tps628640_get_voltage_mv(&buck, &mv), ALP_OK);
    zassert_equal(mv, 1100);

    zassert_equal(tps628640_set_voltage_mv(&buck, 399), ALP_ERR_OUT_OF_RANGE);
    zassert_equal(tps628640_set_voltage_mv(&buck, 1680), ALP_ERR_OUT_OF_RANGE);
    tps628640_deinit(&buck);
}

ZTEST(alp_chips, test_tps628640_control_shadow_rmw)
{
    /* CONTROL is write-only silicon; the driver must compose every
     * write from its shadow so independent knobs don't clobber each
     * other. */
    fake_tps628640_reset();
    alp_i2c_t    *bus = chips_test_bus();
    tps628640_t  buck;
    zassert_equal(tps628640_init(&buck, bus, 0x4D, 600), ALP_OK);

    zassert_equal(tps628640_set_fpwm_mode(&buck, true), ALP_OK);
    uint8_t ctrl = fake_tps628640_get_reg(TPS628640_REG_CONTROL);
    zassert_true((ctrl & TPS628640_CTRL_FPWM_MODE) != 0);
    zassert_true((ctrl & TPS628640_CTRL_SOFTWARE_ENABLE) != 0,
                 "enable bit from the default shadow must survive the FPWM write");

    zassert_equal(tps628640_software_enable(&buck, false), ALP_OK);
    ctrl = fake_tps628640_get_reg(TPS628640_REG_CONTROL);
    zassert_equal(ctrl & TPS628640_CTRL_SOFTWARE_ENABLE, 0);
    zassert_true((ctrl & TPS628640_CTRL_FPWM_MODE) != 0,
                 "FPWM choice must survive the enable write");
    tps628640_deinit(&buck);
}

#endif /* fake_tps628640 */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_optiga))

ZTEST(alp_chips, test_optiga_probe_and_apdu_contract)
{
    fake_optiga_reset();
    alp_i2c_t         *bus = chips_test_bus();
    optiga_trust_m_t  se;
    zassert_equal(optiga_trust_m_init(&se, bus, 0), ALP_OK,
                  "addr 0 selects the OPTIGA_TRUST_M_I2C_ADDR default");
    /* The full info-pack transport is deliberately NOSUPPORT until
     * Infineon's host library is vendored -- lock the contract so a
     * half-implementation can't silently fake success. */
    size_t  rlen = 0;
    uint8_t resp[8];
    zassert_equal(optiga_trust_m_send_apdu(&se, NULL, 0, resp, sizeof(resp), &rlen, 100),
                  ALP_ERR_NOSUPPORT);
    optiga_trust_m_deinit(&se);
}

#endif /* fake_optiga */
```

If any test fails (TDD: run first, watch it fail only where a driver bug exists), fix the DRIVER, not the test — these encodings are datasheet-locked. Exception: if `rv3028c7_set_time` writes via a different register path than 0x00–0x06 sequential, read `chips/rv3028c7/rv3028c7.c` and align the fake's expectations with the driver's actual wire traffic (the BCD values themselves are non-negotiable).

- [ ] **Step 8: Run the chips suite (PASS), clang-format the touched files, commit**

```
git add tests/zephyr/chips
git commit -q -m "test(chips): register-level fakes + ztests for the BRD_I2C ICs -- rv3028c7 BCD/PORF, tmp112 12/13-bit sign extension, 5l35023b strap guard, tps628640 shadow RMW, optiga probe contract"
```

---

### Task 6: Bring-up example `examples/v2n/v2n-brd-i2c-bringup/`

Mirrors `examples/v2n/v2n-rtc-multi-alarm/` exactly. Strictly read-only toward every PMIC.

**Files:**
- Create: `examples/v2n/v2n-brd-i2c-bringup/board.yaml`
- Create: `examples/v2n/v2n-brd-i2c-bringup/CMakeLists.txt`
- Create: `examples/v2n/v2n-brd-i2c-bringup/prj.conf`
- Create: `examples/v2n/v2n-brd-i2c-bringup/testcase.yaml`
- Create: `examples/v2n/v2n-brd-i2c-bringup/src/main.c`
- Create: `examples/v2n/v2n-brd-i2c-bringup/README.md`

- [ ] **Step 1: board.yaml**

```yaml
som:
  sku: E1M-V2N101
preset: e1m-x-evk
cores:
  a55_cluster:
    os: "off"
  m33_sm:
    app: ./src
    peripherals:
      - i2c
chips:
  - rv3028c7
  - tmp112
  - clk_5l35023b
  - act8760
  - da9292
  - tps628640
  - optiga_trust_m
  - gd32g553
diagnostics:
  log_level: info
```

- [ ] **Step 2: CMakeLists.txt** — copy `examples/v2n/v2n-rtc-multi-alarm/CMakeLists.txt` verbatim, changing only the project name to `v2n_brd_i2c_bringup`.

- [ ] **Step 3: prj.conf**

```conf
CONFIG_PRINTK=y
CONFIG_LOG=y
```

- [ ] **Step 4: testcase.yaml**

```yaml
# SPDX-License-Identifier: Apache-2.0
sample:
  name: v2n-brd-i2c-bringup
  description: |
    Patch-day diagnostic for the V2N SoM's BRD_I2C management bus:
    bus-health check + full address scan + read-only probe of every
    populated IC (RTC, temp sensor, clock generator, both PMICs, the
    optional LPDDR buck, the secure element, and the GD32 supervisor
    over its I2C transport), ending in a PASS/FAIL/SKIP table.

tests:
  alp_sdk.examples.v2n.brd_i2c_bringup.native_sim:
    platform_allow:
      - native_sim/native/64
    tags:
      - alp-sdk
      - example
      - v2n
      - i2c
    build_only: true
```

- [ ] **Step 5: src/main.c** — the full teaching artifact:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-brd-i2c-bringup -- patch-day diagnostic for the V2N SoM's
 * BRD_I2C management bus.
 *
 * BRD_I2C is the SoM's housekeeping bus (Renesas RIIC8, master pads
 * P07/P06).  Eight ICs share it -- see the authoritative table in
 * metadata/e1m_modules/E1M-V2N101.yaml:
 *
 *   0x1E  da9292          secondary PMIC (DEEPX rail on V2N-M1)
 *   0x25  act8760 ADD1    primary PMIC: system + Buck1..6 + GPIOs
 *   0x26  act8760 ADD2    primary PMIC: Buck7 + LDO1..6
 *   0x30  optiga_trust_m  secure element
 *   0x40  tmp112          temp sensor  (metadata addr -- but see below!)
 *   0x4D  tps628640       LPDDR4X 0.6 V buck (assembly OPTION)
 *   0x52  rv3028c7        RTC
 *   0x68  clk_5l35023b    clock generator
 *   0x70  gd32g553        IO-MCU bridge, I2C slave transport
 *
 * The flow:
 *   Phase 0 -- bus health: full 0x08..0x77 scan.  Zero ACKs anywhere
 *              means a BUS-level fault (a line held low, missing
 *              pull-ups, wrong pinmux) rather than missing chips;
 *              the report says so explicitly instead of printing
 *              nine cryptic per-device NAKs.
 *   Phase 1 -- per-IC probe, strictly READ-ONLY toward the PMICs:
 *              nothing in this example ever writes a voltage, enable,
 *              or control register.  (The RTC init does clear its
 *              power-on flag and select 24 h mode -- that is the
 *              documented, side-effect-free bring-up handshake.)
 *
 * TMP112 address note: the SoM metadata says 0x40, but TI's TMP112
 * only decodes 0x48..0x4B (ADD0 strap).  Until silicon settles the
 * question this example probes BOTH and reports which one ACKs --
 * whichever way it lands, fix metadata or BOM, not this file first.
 */

#include <stdio.h>
#include <zephyr/kernel.h>

#include <alp/peripheral.h>
#include <alp/chips/rv3028c7.h>
#include <alp/chips/tmp112.h>
#include <alp/chips/clk_5l35023b.h>
#include <alp/chips/act8760.h>
#include <alp/chips/da9292.h>
#include <alp/chips/tps628640.h>
#include <alp/chips/optiga_trust_m.h>
#include <alp/chips/gd32g553.h>

/* One row of the final report. */
typedef enum { R_PASS, R_FAIL, R_SKIP } result_t;

struct report_row {
    const char *name;
    uint8_t     addr;
    result_t    result;
    char        detail[64];
};

#define ROW_MAX 10
static struct report_row rows[ROW_MAX];
static int               row_count;

static void report(const char *name, uint8_t addr, result_t res, const char *fmt, ...)
{
    if (row_count >= ROW_MAX) return;
    struct report_row *r = &rows[row_count++];
    r->name   = name;
    r->addr   = addr;
    r->result = res;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
    va_end(ap);
}

/* A 1-byte read is the least-invasive ACK probe: every chip on this
 * bus tolerates a register-pointer read, and unlike a write it can
 * never alter device state.  The portable backend maps a NAK (or any
 * bus fault) to ALP_ERR_IO. */
static bool acks(alp_i2c_t *bus, uint8_t addr)
{
    uint8_t b = 0;
    return alp_i2c_read(bus, addr, &b, 1) == ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 0: bus health                                                 */
/* ------------------------------------------------------------------ */

static int scan_bus(alp_i2c_t *bus)
{
    int hits = 0;
    printk("Phase 0: scanning 0x08..0x77 ...\n");
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (acks(bus, a)) {
            printk("  ACK at 0x%02X\n", a);
            hits++;
        }
    }
    if (hits == 0) {
        /* The signature of the bus-level fault this example exists
         * to diagnose: with pull-ups healthy and the mux right, at
         * least the PMICs always ACK (they are powered whenever the
         * SoM runs at all).  Zero ACKs = electrical problem. */
        printk("  !! ZERO devices ACK.  This is a BUS-level fault:\n");
        printk("     - a device or short holding SDA/SCL low,\n");
        printk("     - missing/disconnected pull-ups, or\n");
        printk("     - the RIIC8 pinmux not selected on P07/P06.\n");
        printk("     Scope the lines before trusting any result below.\n");
    } else {
        printk("  %d device(s) ACK.\n", hits);
    }
    return hits;
}

/* ------------------------------------------------------------------ */
/* Phase 1: per-IC probes (read-only)                                  */
/* ------------------------------------------------------------------ */

static void probe_rtc(alp_i2c_t *bus)
{
    rv3028c7_t rtc;
    if (rv3028c7_init(&rtc, bus) != ALP_OK) {
        report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL, "no ACK / status read failed");
        return;
    }
    rv3028c7_time_t t;
    if (rv3028c7_get_time(&rtc, &t) == ALP_OK) {
        /* A wildly implausible year usually means the backup supply
         * never charged -- worth knowing on first power-up. */
        report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_PASS,
               "%04u-%02u-%02u %02u:%02u:%02u%s", t.year, t.month, t.day,
               t.hour, t.minute, t.second,
               (t.year < 2026 || t.year > 2099) ? " (time not set)" : "");
    } else {
        report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL, "probe OK but time read failed");
    }
    rv3028c7_deinit(&rtc);
}

static void probe_tmp112(alp_i2c_t *bus)
{
    /* Address discrepancy under test: metadata says 0x40, the TMP112
     * datasheet says 0x48..0x4B.  Find where (and whether) it ACKs. */
    const uint8_t candidates[] = {0x40, TMP112_I2C_ADDR_GND, 0x49, 0x4A, 0x4B};
    uint8_t       found        = 0;
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        if (acks(bus, candidates[i])) {
            found = candidates[i];
            break;
        }
    }
    if (found == 0) {
        report("tmp112 temp", 0x40, R_FAIL, "no ACK at 0x40 nor 0x48..0x4B");
        return;
    }
    if (found == 0x40) {
        /* ACKs at the metadata address -- but the driver (faithfully
         * to the datasheet) refuses 0x40.  Surface the conflict; do
         * NOT quietly loosen the driver. */
        report("tmp112 temp", 0x40, R_FAIL,
               "ACKs at 0x40 -- not a TMP112 address; fix metadata or BOM");
        return;
    }
    tmp112_t sens;
    if (tmp112_init(&sens, bus, found) != ALP_OK) {
        report("tmp112 temp", found, R_FAIL, "ACKs but CONF fingerprint mismatch");
        return;
    }
    int32_t mc = 0;
    if (tmp112_read_temp_milli_c(&sens, &mc) == ALP_OK) {
        report("tmp112 temp", found, R_PASS, "%d.%03d degC", (int)(mc / 1000),
               (int)((mc < 0 ? -mc : mc) % 1000));
    } else {
        report("tmp112 temp", found, R_FAIL, "temperature read failed");
    }
    tmp112_deinit(&sens);
}

static void probe_clkgen(alp_i2c_t *bus)
{
    clk_5l35023b_t clk;
    if (clk_5l35023b_init(&clk, bus, CLK_5L35023B_I2C_ADDR_DEFAULT) != ALP_OK) {
        report("5l35023b clk", CLK_5L35023B_I2C_ADDR_DEFAULT, R_FAIL,
               "no ACK or address-strap mismatch");
        return;
    }
    uint8_t dash = 0;
    (void)clk_5l35023b_read_dashcode_id(&clk, &dash);
    report("5l35023b clk", CLK_5L35023B_I2C_ADDR_DEFAULT, R_PASS, "dash code 0x%02X", dash);
    clk_5l35023b_deinit(&clk);
}

static void probe_act8760(alp_i2c_t *bus)
{
    act8760_t pmic;
    if (act8760_init(&pmic, bus) != ALP_OK) {
        report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL,
               "ADD1 (0x25) or ADD2 (0x26) missing");
        return;
    }
    act8760_status_t st;
    if (act8760_get_status(&pmic, &st) == ALP_OK) {
        report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_PASS,
               "both slaves; status 0x%02X%s%s", st.raw,
               st.thermal_warning ? " TWARN!" : "",
               st.vsys_warning ? " VSYSWARN!" : "");
    } else {
        report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL, "status read failed");
    }
    act8760_deinit(&pmic);
}

static void probe_da9292(alp_i2c_t *bus)
{
    da9292_t pmic;
    if (da9292_init(&pmic, bus, DA9292_I2C_ADDR_V2N) != ALP_OK) {
        report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL, "no ACK / bad DEV_ID");
        return;
    }
    da9292_status_t st;
    if (da9292_get_status(&pmic, &st) == ALP_OK) {
        report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_PASS,
               "dev 0x%02X rev 0x%02X  CH1 PG=%d  CH2 PG=%d", pmic.dev_id,
               pmic.rev_id, st.ch1_pg, st.ch2_pg);
    } else {
        report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL, "status read failed");
    }
    da9292_deinit(&pmic);
}

static void probe_tps628640(alp_i2c_t *bus)
{
    /* Assembly option: absent on most V2N base builds.  A NAK here
     * is expected, not a failure. */
    if (!acks(bus, 0x4D)) {
        report("tps628640", 0x4D, R_SKIP, "not populated (assembly option)");
        return;
    }
    tps628640_t buck;
    if (tps628640_init(&buck, bus, 0x4D, 600) != ALP_OK) {
        report("tps628640", 0x4D, R_FAIL, "ACKs but VOUT1 read failed");
        return;
    }
    uint16_t mv = 0;
    (void)tps628640_get_voltage_mv(&buck, &mv);
    report("tps628640", 0x4D, R_PASS, "VOUT1 = %u mV", mv);
    tps628640_deinit(&buck);
}

static void probe_optiga(alp_i2c_t *bus)
{
    optiga_trust_m_t se;
    if (optiga_trust_m_init(&se, bus, 0) != ALP_OK) {
        report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_FAIL, "no ACK on I2C_STATE");
        return;
    }
    report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_PASS, "I2C_STATE readable");
    optiga_trust_m_deinit(&se);
}

static void probe_gd32(alp_i2c_t *bus)
{
    /* The supervisor MCU speaks the bridge protocol over BRD_I2C as
     * its management transport (SPI is the fast path).  init() runs
     * PING + GET_VERSION and enforces the protocol-major match. */
    gd32g553_t mcu;
    if (gd32g553_init(&mcu, NULL, bus, GD32G553_BRIDGE_DEFAULT_I2C_ADDR) != ALP_OK) {
        report("gd32g553 bridge", GD32G553_BRIDGE_DEFAULT_I2C_ADDR, R_FAIL,
               "PING/GET_VERSION failed (firmware running? major match?)");
        return;
    }
    report("gd32g553 bridge", GD32G553_BRIDGE_DEFAULT_I2C_ADDR, R_PASS,
           "fw v%u.%u.%u over I2C", mcu.version.major, mcu.version.minor,
           mcu.version.patch);
    gd32g553_deinit(&mcu);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printk("\n=== V2N BRD_I2C bring-up diagnostic ===\n");

    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,      /* BRD_I2C = bus 0 on the V2N M33 target */
        .bitrate_hz = 400000u, /* every IC on this bus is FM-capable    */
    });
    if (bus == NULL) {
        printk("FATAL: alp_i2c_open(bus 0) failed -- check the board "
               "overlay wires the alp-i2c0 alias.\n");
        return 1;
    }

    int hits = scan_bus(bus);

    printk("\nPhase 1: per-IC probes (read-only)\n");
    probe_rtc(bus);
    probe_tmp112(bus);
    probe_clkgen(bus);
    probe_act8760(bus);
    probe_da9292(bus);
    probe_tps628640(bus);
    probe_optiga(bus);
    probe_gd32(bus);

    printk("\n==== BRD_I2C report ====\n");
    printk("%-16s %-5s %-5s %s\n", "device", "addr", "res", "detail");
    int fails = 0;
    for (int i = 0; i < row_count; i++) {
        const char *res = rows[i].result == R_PASS ? "PASS"
                        : rows[i].result == R_SKIP ? "SKIP" : "FAIL";
        if (rows[i].result == R_FAIL) fails++;
        printk("%-16s 0x%02X  %-5s %s\n", rows[i].name, rows[i].addr, res,
               rows[i].detail);
    }
    printk("========================\n");
    if (hits == 0) {
        printk("VERDICT: bus-level fault -- fix the electrical problem first.\n");
    } else if (fails == 0) {
        printk("VERDICT: BRD_I2C fully alive.\n");
    } else {
        printk("VERDICT: bus alive, %d device(s) failing -- see rows above.\n", fails);
    }

    alp_i2c_close(bus);
    return 0;
}
```

(Add `#include <stdarg.h>` next to `<stdio.h>` — `report()` is variadic.)

- [ ] **Step 6: README.md**

```markdown
# v2n-brd-i2c-bringup

Patch-day diagnostic for the V2N SoM's BRD_I2C management bus
(Renesas RIIC8). Scans the bus, distinguishes a bus-level electrical
fault (line held low / missing pull-ups / wrong pinmux) from
per-device failures, then probes every populated IC read-only and
prints a PASS/FAIL/SKIP table.

Probed devices (addresses per `metadata/e1m_modules/E1M-V2N101.yaml`):
DA9292 (0x1E), ACT88760 (0x25+0x26), OPTIGA Trust M (0x30), TMP112
(0x40 per metadata — the example also tries the datasheet 0x48..0x4B
range and reports the discrepancy), TPS628640 (0x4D, assembly
option → SKIP when absent), RV-3028-C7 (0x52), 5L35023B (0x68), and
the GD32G553 supervisor over its I2C bridge transport (0x70).

The example never writes a PMIC voltage, enable, or control
register. Build for the V2N M33 target via the standard alp flow;
the twister scenario is build-only on native_sim.
```

- [ ] **Step 7: Build it under twister** (same command as Task 1 Step 3 but `--testsuite-root .../examples`), expect the new scenario green. If `alp_project.py --emit zephyr-conf` rejects any `chips:` entry, check the chip_id spelling against `metadata/chips/*.yaml` filenames.

- [ ] **Step 8: Commit**

```
git add examples/v2n/v2n-brd-i2c-bringup
git commit -q -m "feat(examples): v2n-brd-i2c-bringup patch-day diagnostic -- bus-health scan + read-only probe of all 8 BRD_I2C ICs incl. the GD32 I2C transport; surfaces the tmp112 0x40-vs-0x48 metadata discrepancy instead of hiding it"
```

---

### Task 7: CHANGELOG + doc sweep

**Files:**
- Modify: `CHANGELOG.md` (under `[Unreleased]`)
- Possibly modify: any doc table claiming act8760/da9292 driver status (grep first)

- [ ] **Step 1: CHANGELOG entries** under `[Unreleased]`:

```markdown
### Added
- `examples/v2n/v2n-brd-i2c-bringup`: read-only patch-day diagnostic for the
  V2N BRD_I2C bus (bus-health scan + per-IC probe table, GD32 I2C transport
  included).
- Register-level i2c-emul fakes + ztests for the BRD_I2C chip drivers
  (rv3028c7, tmp112, clk_5l35023b, act8760, da9292, tps628640,
  optiga_trust_m); the chips-suite fake infrastructure is re-enabled under
  the Zephyr v4.4.0 pin.

### Fixed
- `act8760`: replaced the provisional (guessed) register map with the
  verified one — two-slave addressing, per-rail VSET0 tile offsets (Buck7
  lives on ADD2 with the LDOs), read-modify-write VSET accessors replacing
  the `ALP_ERR_NOSUPPORT` stubs, and a corrected system-status decode
  (TWARN is bit 5; SYSDAT was never in register 0x00).
- `da9292`: PMC_STATUS_00 bit layout verified against datasheet Rev 2.2
  Table 14 (the existing decode was correct); TODO retired with citations.
```

- [ ] **Step 2: Doc sweep** — `grep -rn "act8760\|da9292\|ACT88760\|DA9292" docs/` and update any stale driver-status claims (e.g. "VSET accessors pending verification"). Expected: small or zero diff.

- [ ] **Step 3: Commit**

```
git add CHANGELOG.md docs
git commit -q -m "docs: changelog + status sweep for the BRD_I2C driver-readiness slice"
```

---

### Task 8: Full local gates, then push to dev

- [ ] **Step 1: Full twister scope** (all three roots — never a subset):

```
wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
  export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
  export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
  export ZEPHYR_TOOLCHAIN_VARIANT=host && \
  python3 zephyr/scripts/twister \
    --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/zephyr \
    --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
    --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
    -p native_sim/native/64 -O /tmp/tw-full'
```

Read the summary from `/tmp/tw-full/twister.log` (no pipes). Do NOT run gen_soc_caps/generated-files regen concurrently (the `ALP_SOC_REF_STR` flake).

- [ ] **Step 2: pytest gates** (Windows, `py -3.14`):

```
py -3.14 -m pytest tests/scripts -q
```

- [ ] **Step 3: clang-format diff-only check** (WSL, CI-faithful):

```
wsl -d Ubuntu -- bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && git diff -U0 origin/dev -- "*.c" "*.h" | clang-format-diff-14 -p1 -binary /usr/bin/clang-format-14'
```

Empty output = clean. Non-empty: `clang-format-14 -style=file -i <file>` the offenders and amend via a follow-up commit (never `--amend`).

- [ ] **Step 4: doc-lint** (if the repo gate exists in `.github/workflows`, reproduce per `running-local-ci` skill; the full-repo walk is fast since the `git ls-files` fix).

- [ ] **Step 5: Push to dev**

```
git push origin dev
```

(`dev` is the untested-integration branch; dev→main waits for bench validation of the bring-up example on patched silicon.)

---

## Out of scope (do not let the implementation drift into these)

- OPTIGA full APDU/info-pack transport (separate vendoring project)
- GD32 bridge firmware changes
- act8760 VSET1–3 DVS slots, BAND_SEL bank switching, typed mV helpers
- Writing PMIC registers from the example
- Fixing the tmp112 0x40 metadata entry (silicon decides; the example reports)
- Issue #90 (pr-twister `tests/unit` gap) — separate follow-up
