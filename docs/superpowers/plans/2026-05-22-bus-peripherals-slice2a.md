# Slice 2 Session A — Bus Peripherals Registry Migration (i2c / spi / uart)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each peripheral block (Task A / B / C) is the unit of subagent dispatch — dispatch ONE subagent per peripheral, sequentially.

**Goal:** Migrate the three bus peripherals — `alp_i2c_*`, `alp_spi_*`, `alp_uart_*` — onto the backend registry shipped in Slice 0 (PR #17), following the canonical 8-commit handle-based template proven across RTC/WDT/Counter/QEnc in Slice 4a (PR #19) and Counter/QEnc in particular.

**Architecture:**
- Public functions in `<alp/peripheral.h>` keep their existing signatures because that's the right portable surface — NOT because of any ABI-stability obligation. alp-sdk has no active customers; migrations delete legacy code cleanly with no shims, tombstones, or provenance comments (see memory `feedback_no_legacy_compat`).
- Per peripheral: 8 commits, plus a small style-cleanup commit at the end to strip any legacy-provenance references that accumulated mid-migration. No GD32 bridge variant — i2c/spi/uart on V2N use native Zephyr drivers, not the GD32 supervisor MCU.
- Vendor-extension audit (master spec §3): **no vendor ext** for any of the three. Every knob currently used (bitrate_hz, SPI mode/freq/cs, UART baud/parity) has a portable equivalent in Zephyr's driver class and the existing `alp_<bus>_config_t`. Audit verdict: portable-only.

**Tech Stack:** C11, Zephyr 3.6 driver classes (i2c, spi, uart), `<alp/backend.h>` registry macros (`ALP_BACKEND_DEFINE_CLASS`, `ALP_BACKEND_REGISTER`, `alp_backend_select`), ztest unit tests.

---

## Pre-flight — branch & base (operator does once)

- [ ] **Branch off `feat/backend-registry-foundation` (Slice 0, PR #17)**

```
git checkout feat/backend-registry-foundation
git checkout -b feat/backend-registry-core-peripherals
```

Slice 2 depends only on the Slice 0 registry mechanics (`ALP_BACKEND_DEFINE_CLASS`, `ALP_BACKEND_REGISTER`, `alp_backend_select` — all live on `feat/backend-registry-foundation`, NOT yet merged to main). The slice is independent of #18/#19/#26/#27/#28/#29 — so branching off Slice 0 directly avoids piggybacking on the full stacked tower. The GitHub PR opens with `--base feat/backend-registry-foundation` and will rebase onto `main` automatically once #17 merges.

- [ ] **Sanity check: registry foundation symbols are present**

```
grep -nE "ALP_BACKEND_DEFINE_CLASS|ALP_BACKEND_REGISTER" include/alp/backend.h
```

Expected: matches present. If absent, the wrong base is checked out — STOP.

---

## Canonical 8-commit template (shared across Task A / B / C)

Every peripheral migration produces this exact commit sequence. Subsequent tasks reference this section by step number; deviation requires a written justification in the task brief.

| # | Commit subject                                                       | Files touched                                                                                                       |
|---|----------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|
| 1 | `feat(<p>): declare alp_<p>_capabilities() public getter`            | `include/alp/peripheral.h` only — adds one `alp_capabilities_t` getter declaration. ABI grows, never shrinks.       |
| 2 | `feat(<p>): private backend ops vtable + handle layout`              | `src/backends/<p>/<p>_ops.h` (new). Defines `alp_<p>_ops_t`, `alp_<p>_backend_state_t`, full `struct alp_<p>` body. |
| 3 | `feat(<p>): class dispatcher + handle pool`                          | `src/<p>_dispatch.c` (new). Owns the public `alp_<p>_*` surface, calls `alp_backend_select`, pools `CONFIG_ALP_SDK_MAX_<P>_HANDLES`. |
| 4 | `feat(<p>): portable Zephyr driver-class backend`                    | `src/backends/<p>/zephyr_drv.c` (new). Lifts the existing logic from `src/zephyr/peripheral_<p>.c`, registers at `silicon_ref="*"` priority 100. |
| 5 | `feat(<p>): SW fallback backend (<short description>)`               | `src/backends/<p>/sw_fallback.c` (new). Wildcard, priority 0. Test-only seam — always loses to `zephyr_drv` on real silicon. |
| 6 | `refactor(<p>): drop legacy peripheral_<p>.c + handle pool plumbing` | `git rm src/zephyr/peripheral_<p>.c`. Trim the legacy `alp_z_<p>_pool_acquire` / `_release` from `src/zephyr/handles.c` (the **invocation** of `DEFINE_POOL(<p>, ...)` only — leave the macro definition untouched per session lesson #5). |
| 7 | `build(<p>): wire registry sources + add CONFIG_ALP_SDK_<P>_SW_FALLBACK` | `zephyr/CMakeLists.txt` (drop legacy line, add dispatcher + zephyr_drv + ifdef'd sw_fallback) and `zephyr/Kconfig` (new `CONFIG_ALP_SDK_<P>_SW_FALLBACK` knob mirroring the COUNTER one). |
| 8 | `test(<p>): unit-test harness for the <p> registry`                  | `tests/unit/<p>_registry/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_<p>_registry.c}` (new). 5–7 ztest cases (see "Test harness shape" below). |
| 9 | `style(<p>): drop legacy-provenance comments and obsolete README row`   | Optional cleanup commit if mid-migration you wrote "Lifted from src/zephyr/peripheral_<p>.c" comments, "moved to..." / "removed (Slice 2a)" tombstones in `src/zephyr/handles.h`, or left rows in `src/zephyr/README.md` pointing at the now-deleted legacy file. STRIP them. alp-sdk has no active customers — no provenance trail needs to survive in source. (See `feedback_no_legacy_compat` memory.) |

### Canonical references (git show these before writing any code)

For every subagent brief, the canonical example is **Counter** (Slice 4a, 8 commits, clean handle-based, no bridge variant for i2c/spi/uart parity):

```
git show b52e190   # Step 1 — counter capabilities getter
git show a320943   # Step 2 — counter ops vtable + handle
git show d218efe   # Step 3 — counter dispatcher
git show 81e318f   # Step 4 — counter Zephyr backend
git show eb5e753   # Step 5 — counter SW fallback
git show 982f717   # Step 6 — counter legacy removal + handles.c trim
git show 36c1807   # Step 7 — counter cmake + Kconfig
git show f7c9532   # Step 8 — counter registry test harness
```

**Skip the bridge commit (`git show 4fc7fe7`)** — it is not part of Slice 2 Session A because the bus peripherals do not route through the GD32 supervisor MCU on V2N.

The dispatcher pattern at `src/rtc_dispatch.c:46-68` is the canonical `alp_<p>_open` shape (select → probe → alloc → open → cache caps).
The ops header pattern at `src/backends/rtc/rtc_ops.h` is the canonical vtable shape.

### Test harness shape (Step 8 — applies to all three)

`tests/unit/<p>_registry/src/test_<p>_registry.c` must include these ztest cases:

1. `selector_prefers_zephyr_drv_on_alif` — verifies priority 100 > priority 0 with `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y`.
2. `selector_falls_back_to_sw_when_only_fallback_present` — gates the slice's `CONFIG_ALP_SDK_<P>_SW_FALLBACK=y` and verifies the wildcard match wins by default.
3. `open_returns_null_on_invalid_bus_id` — `bus_id >= ALP_SOC_<P>_COUNT` (or per-peripheral validation rules) yields NULL + `ALP_ERR_OUT_OF_RANGE`.
4. `capabilities_getter_matches_backend_base_caps` — `alp_<p>_capabilities(h)->flags` round-trips the backend's `base_caps`.
5. `close_releases_handle` — pool size N → open N+1 → (N+1)th open returns NULL → close one → next open succeeds.
6. *(uart only)* `rx_ringbuf_round_trip` — preserve the existing ringbuf contract since it is a Kconfig-gated, optional add-on; the registry migration must not regress it.

Each test file must `#include <zephyr/ztest.h>` and register against the existing per-suite name pattern observed in `tests/unit/counter_registry/src/test_counter_registry.c`.

`prj.conf`:
```
CONFIG_ZTEST=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
CONFIG_ALP_SDK_<P>_SW_FALLBACK=y
```

`testcase.yaml`: copy verbatim from the counter_registry one with the `<p>` substitution; the runtime board pin (`platform_allow:` / `tags:`) stays at `native_sim qemu_cortex_m3`.

### Final reporting checklist (every subagent must include this in its summary)

The subagent finishes by reporting:
1. **Commits produced** — exact 8 SHAs in order, with subject lines.
2. **`git status` confirms clean tree.** No staged/unstaged changes left over.
3. **`grep -rn "peripheral_<p>" src/ zephyr/`** returns ZERO matches (verifies Step 6 cleanly retired the legacy file).
4. **Twister smoke**: `west twister -T tests/unit/<p>_registry -p native_sim --inline-logs` PASSES locally if the environment has west; otherwise the subagent reports "twister not available in subagent env — operator to run".
5. **Vendor-ext audit verdict** quoted: `"no vendor-ext header introduced; <p>'s portable surface fully covers Zephyr driver-class equivalents — see master spec §3"`.
6. **Lessons re-confirmed** from session lessons:
   - alpCaner-only commits, no Co-Authored-By: Claude footer
   - did not touch `src/yocto/peripheral_<p>.c`
   - did not modify the `DEFINE_POOL` macro definition; only deleted the invocation line
   - did not stage `include/alp/boards/alp_e1m_evk_routes.h` or `include/alp/soc_caps.h` if their diffs are noise

---

## Task A: i2c migration  ⟶  one subagent

**Files (Step 1–8 mapping):**
- Create: `src/backends/i2c/i2c_ops.h`, `src/backends/i2c/zephyr_drv.c`, `src/backends/i2c/sw_fallback.c`, `src/i2c_dispatch.c`, `tests/unit/i2c_registry/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_i2c_registry.c}`
- Modify: `include/alp/peripheral.h` (Step 1 only — add the capabilities getter), `src/zephyr/handles.c` (Step 6 — drop the i2c pool invocation line), `zephyr/CMakeLists.txt` (Step 6 + 7), `zephyr/Kconfig` (Step 7)
- Delete: `src/zephyr/peripheral_i2c.c` (Step 6)
- Test: `tests/unit/i2c_registry/src/test_i2c_registry.c`

**Public surface to keep ABI-stable (verbatim from `include/alp/peripheral.h:237-308`):**
- `alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg)`
- `alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)`
- `alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)`
- `alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr, const uint8_t *wdata, size_t wlen, uint8_t *rdata, size_t rlen)`
- `void alp_i2c_close(alp_i2c_t *bus)`

**Ops vtable (Step 2):** five entries — `open / write / read / write_read / close`. Mirrors `alp_rtc_ops` 1-to-1.

**Legacy logic to lift into `zephyr_drv.c` (Step 4):** all 125 lines of `src/zephyr/peripheral_i2c.c` (the file uses no `#ifdef BRIDGE` branches — straight Zephyr i2c API). Keep `alp_to_zephyr_bitrate_flags`, `errno_to_alp`, and the `alp_i2c_devs[]` DT-alias resolution table as `static` helpers inside the backend file. Drop the `alp_z_i2c_pool_acquire/release` calls — pooling now lives in `i2c_dispatch.c`.

**SW fallback shape (Step 5):** in-memory loopback — `write` stashes the last frame in a static buffer; `read` returns that frame zero-padded. Mirrors `src/backends/counter/sw_fallback.c` deterministic-state philosophy. Priority 0, `silicon_ref="*"`.

**Kconfig name (Step 7):** `ALP_SDK_I2C_SW_FALLBACK` (no `PERIPH_` infix — Slice 4a moved off that prefix). Default `n` on real silicon; tests enable it explicitly in `prj.conf`.

- [ ] **Step A.0** — Operator reads this entire Task A block, then dispatches **one** subagent with the brief at the end of this section.
- [ ] **Step A.1–A.8** — Subagent produces the 8 commits per the canonical template.
- [ ] **Step A.9** — Operator reviews diff (`git log main..HEAD --reverse --stat`) before dispatching Task B.

**Subagent brief (paste verbatim when dispatching):**

> You are migrating `<alp/peripheral.h>`'s i2c surface onto the backend registry in this branch (`feat/backend-registry-core-peripherals`, already checked out, started from `main`).
>
> Read these canonical commits before touching code:
> `git show b52e190 a320943 d218efe 81e318f eb5e753 982f717 36c1807 f7c9532`
> (the Counter migration from Slice 4a — your exact 8-commit template, minus the bridge step which does not apply to i2c).
>
> Also read `src/rtc_dispatch.c`, `src/backends/rtc/rtc_ops.h`, and the legacy `src/zephyr/peripheral_i2c.c` to anchor the shapes.
>
> Produce the 8 commits listed in `docs/superpowers/plans/2026-05-22-bus-peripherals-slice2a.md` Task A, in order, each with a Conventional Commits subject. Author: alpCaner only — no Co-Authored-By: Claude footer.
>
> Constraints (re-stated for your convenience, full list lives in the plan):
> - Do NOT touch `src/yocto/peripheral_i2c.c` — Yocto-side migration is tracked separately in issue #33.
> - Do NOT modify the `DEFINE_POOL` macro definition in `src/zephyr/handles.c`; only delete the `DEFINE_POOL(i2c, ...)` invocation line.
> - Do NOT stage `include/alp/boards/alp_e1m_evk_routes.h` or `include/alp/soc_caps.h` if their diffs are CRLF-only.
> - Keep the existing `alp_i2c_open/write/read/write_read/close` signatures because they're the right portable surface — NOT because of ABI stability. alp-sdk has no active customers.
> - The new `CONFIG_ALP_SDK_I2C_SW_FALLBACK` knob defaults to `n` on real-silicon boards; only the new `tests/unit/i2c_registry/prj.conf` and any test build turns it on.
> - NO legacy-provenance comments: no "Lifted from src/zephyr/peripheral_i2c.c" headers in new files, no "moved to..." / "removed (Slice 2a)" tombstones in `src/zephyr/handles.h`. Update `src/zephyr/README.md` in commit 7 to drop the obsolete `peripheral_i2c.c` row.
>
> Finish with the "Final reporting checklist" from the plan's canonical-template section.

---

## Task B: spi migration  ⟶  one subagent (dispatch AFTER Task A merges to the local branch)

**Files (Step 1–8 mapping):**
- Create: `src/backends/spi/spi_ops.h`, `src/backends/spi/zephyr_drv.c`, `src/backends/spi/sw_fallback.c`, `src/spi_dispatch.c`, `tests/unit/spi_registry/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_spi_registry.c}`
- Modify: `include/alp/peripheral.h`, `src/zephyr/handles.c`, `zephyr/CMakeLists.txt`, `zephyr/Kconfig`
- Delete: `src/zephyr/peripheral_spi.c`
- Test: `tests/unit/spi_registry/src/test_spi_registry.c`

**Public surface to keep ABI-stable (`include/alp/peripheral.h:314-387`):**
- `alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)`
- `alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)`
- `alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)`
- `alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)`
- `void alp_spi_close(alp_spi_t *bus)`

**Ops vtable (Step 2):** `open / transceive / close`. `write` and `read` stay in `spi_dispatch.c` as thin one-liners forwarding to `transceive` (mirrors the legacy file's `alp_spi_write / _read` → `alp_spi_transceive` delegation). Note: the SPI handle holds three Zephyr-specific fields (`struct spi_config zspi_cfg`, `struct spi_cs_control cs_ctrl`, `struct gpio_dt_spec cs_spec`, `bool cs_present`) — these move into `alp_spi_backend_state_t.be_data` as an opaque pointer or into `struct alp_spi` directly (`alp_rtc.state` precedent allows either; pick "directly in `struct alp_spi`" since these are CS-resolution metadata, not backend-private).

**Legacy logic to lift (Step 4):** all 142 lines of `src/zephyr/peripheral_spi.c`. Special-case: the `alp_z_gpio_resolve` extern is shared GPIO-side plumbing — leave it alone, just `extern` it into `zephyr_drv.c` the same way.

**SW fallback shape (Step 5):** loopback — `transceive(tx, rx, len)` copies `tx` into `rx` when both are non-NULL. Priority 0, `silicon_ref="*"`. Deterministic, no external state.

**Kconfig name:** `ALP_SDK_SPI_SW_FALLBACK`.

- [ ] **Step B.0** — Operator confirms Task A is locally committed (eight commits ahead of `main`) before dispatching.
- [ ] **Step B.1–B.8** — Subagent produces the 8 commits.
- [ ] **Step B.9** — Operator reviews diff (`git log <task-A-tip>..HEAD --reverse --stat`).

**Subagent brief:** copy Task A's brief verbatim, substituting `i2c` → `spi`. Add one extra constraint: *"Re-use `alp_z_gpio_resolve` via `extern` declaration — do NOT define a second GPIO-resolution helper inside the SPI backend."*

---

## Task C: uart migration  ⟶  one subagent (dispatch AFTER Task B is committed)

**Files (Step 1–8 mapping):**
- Create: `src/backends/uart/uart_ops.h`, `src/backends/uart/zephyr_drv.c`, `src/backends/uart/sw_fallback.c`, `src/uart_dispatch.c`, `tests/unit/uart_registry/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_uart_registry.c}`
- Modify: `include/alp/peripheral.h`, `src/zephyr/handles.c`, `zephyr/CMakeLists.txt`, `zephyr/Kconfig`
- Delete: `src/zephyr/peripheral_uart.c`
- Test: `tests/unit/uart_registry/src/test_uart_registry.c`

**Public surface to keep ABI-stable (`include/alp/peripheral.h:393-451` for the core, `:472-531` for the rx_ringbuf add-on):**
- `alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)`
- `alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len)`
- `alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len, uint32_t timeout_ms)` *(or the existing signature — keep verbatim)*
- `void alp_uart_close(alp_uart_t *port)`
- All `alp_uart_rx_ringbuf_*` helpers (Kconfig-gated by `CONFIG_ALP_SDK_UART_RX_RINGBUF`)

**Ops vtable (Step 2):** `open / write / read / close`. The rx_ringbuf surface stays a *post-open add-on* — it attaches to an already-open `alp_uart_t` via Zephyr's `uart_callback_set`. The ringbuf state moves into `struct alp_uart` directly (not the ops vtable) because it's a portable add-on, not a backend-replaceable concern.

**Legacy logic to lift (Step 4):** all 273 lines of `src/zephyr/peripheral_uart.c`. The biggest of the three. The rx_ringbuf helpers stay alongside `zephyr_drv.c` (they wrap Zephyr's `uart_callback_set` / `uart_rx_enable` — Zephyr-specific). Decision: keep them in `src/backends/uart/zephyr_drv.c` and gate with `#ifdef CONFIG_ALP_SDK_UART_RX_RINGBUF`. The SW fallback does NOT need to implement them — guard the public `alp_uart_rx_ringbuf_*` symbols with `IS_ENABLED(CONFIG_ALP_SDK_UART_RX_RINGBUF)` and have the dispatcher return `ALP_ERR_NOSUPPORT` when the active backend is the sw_fallback.

**SW fallback shape (Step 5):** static circular buffer (32B) — `write` appends, `read` drains. Priority 0, `silicon_ref="*"`.

**Kconfig name:** `ALP_SDK_UART_SW_FALLBACK`.

**Extra Step 8 test case (uart only):** `rx_ringbuf_round_trip` — open uart with the SW fallback active, attach a ringbuf, write 8 bytes, drain 8 bytes from the ringbuf, assert byte-for-byte match. This guards the Kconfig-gated optional surface.

- [ ] **Step C.0** — Operator confirms Tasks A + B are locally committed.
- [ ] **Step C.1–C.8** — Subagent produces the 8 commits.
- [ ] **Step C.9** — Operator reviews diff.

**Subagent brief:** copy Task A's brief verbatim, substituting `i2c` → `uart`. Add two extra constraints:
1. *"The `alp_uart_rx_ringbuf_*` add-on surface stays a portable wrapper inside `src/backends/uart/zephyr_drv.c`, gated by `CONFIG_ALP_SDK_UART_RX_RINGBUF`. The SW fallback does not implement it — dispatcher returns `ALP_ERR_NOSUPPORT` when the ringbuf is attached on a sw_fallback backend."*
2. *"Your registry test harness MUST include the `rx_ringbuf_round_trip` case in addition to the standard six."*

---

## Task D: ABI snapshot regen + slice-final commit  ⟶  operator (no subagent)

After Tasks A + B + C are committed and you have **24 new commits** on `feat/backend-registry-core-peripherals`:

- [ ] **Step D.1** — Regenerate the v0.5 ABI snapshot

```
python3 scripts/extract_abi.py --in include/ --out docs/abi/v0.5-snapshot.json
git diff docs/abi/v0.5-snapshot.json
```

Expected diff: three new entries (`alp_i2c_capabilities`, `alp_spi_capabilities`, `alp_uart_capabilities`). No other ABI changes — the per-peripheral `alp_<bus>_open/read/write/close` signatures must be byte-for-byte preserved.

If unexpected diff appears: STOP and inspect the offending signature change. The migration should never silently widen or rename an existing public function.

- [ ] **Step D.2** — Commit the snapshot

```
git add docs/abi/v0.5-snapshot.json
git commit -m "chore(abi): regenerate v0.5 snapshot for bus-peripherals slice

Adds the three new capability getters introduced by Slice 2 Session A:
  alp_i2c_capabilities, alp_spi_capabilities, alp_uart_capabilities.

No other ABI changes -- existing alp_i2c_*/alp_spi_*/alp_uart_*
signatures are byte-for-byte preserved by the registry migration."
```

- [ ] **Step D.3** — Push and open PR

```
git push -u origin feat/backend-registry-core-peripherals
gh pr create --base feat/backend-registry-foundation \
  --title "feat(periph): i2c/spi/uart registry migration (Slice 2 Session A)" \
  --body-file docs/superpowers/plans/2026-05-22-bus-peripherals-slice2a.md
```

PR body references this plan plus the master spec §3 audit verdict (portable-only, no vendor ext).

---

## Self-review (operator runs once before opening PR)

- **Spec coverage** — Master spec backend-registry §1-3 covers the registry mechanics; this plan implements them for i2c/spi/uart per the canonical Counter template. ✅
- **Placeholder scan** — no `TBD`, `TODO`, `implement later` markers in this plan. ✅
- **Type consistency** — `alp_<p>_ops_t`, `alp_<p>_backend_state_t`, `struct alp_<p>`, `CONFIG_ALP_SDK_MAX_<P>_HANDLES`, `CONFIG_ALP_SDK_<P>_SW_FALLBACK` — all consistent across Task A/B/C. ✅
- **Vendor-ext audit** — captured per peripheral as "no vendor-ext header introduced". ✅
- **ABI preservation** — Step D.1 diff check enforces this. ✅
- **Out-of-scope items deferred** — Yocto-side migration (#33), bridge-selection harness (#32), DEFINE_POOL macro cleanup (#31) all explicitly excluded from this slice. ✅

## Out of scope (do NOT do in this slice)

1. `src/yocto/peripheral_<bus>.c` — tracked in #33.
2. Bridge-selection tests — tracked in #32; bus peripherals don't use the GD32 bridge anyway.
3. `DEFINE_POOL` macro cleanup in `src/zephyr/handles.c` — tracked in #31. The invocation-line-only deletion in Step 6 is the bare minimum needed; do not refactor the macro itself.
4. `peripheral.h` non-bus surfaces (gpio/pwm/i2s/can/dac) — those are Session B / later.
5. Inference, storage, vendor-ext promotions — separate slices.
