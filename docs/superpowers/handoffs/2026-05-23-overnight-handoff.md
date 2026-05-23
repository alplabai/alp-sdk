# Overnight session handoff — 2026-05-23

Session ran 2026-05-22 ~22:48 → 2026-05-23 ~02:30 (≈3.5 h wall-clock).
Started from `feat/backend-registry-core-peripherals` (PR #35 head). Local main now sync'd to `origin/main` at `b1041db`.

## TL;DR

- Phase 1 partial: 3 of 11 PRs merged (#14, #15, #36-recreated-for-#16). The remaining 8 (#17 + dependents) are conflict-blocked on #17.
- Phase 2 full: 5 new slice PRs opened, 60 fresh commits authored by alpCaner (no Claude footer, no forbidden files staged).
- 2 new tracking issues filed (#58, #59) for NOT_IMPLEMENTED inference stubs.
- Branch protection restored on main.

## Phase 1 — merge stack (PARTIAL)

### Merged
| PR | Title | Notes |
|---|---|---|
| #14 | feat(cap): capability-api | merged clean |
| #15 | feat(validator): board.yaml diagnostics | merged clean; deleting source branch auto-closed #16 (race with auto-retarget) |
| #16 | feat(cli): alp init/run/validate | auto-CLOSED when #15's source branch was deleted (GitHub auto-retarget lost the race vs `--delete-branch`) |
| **#36** | **feat(cli): alp init/run/validate (recreated from #16)** | **merged clean** |

### Blocked on #17 conflict
PR #17 has a REAL merge conflict against current main in 3 files:
- `docs/abi/v0.5-snapshot.json` (regenerable)
- `include/alp/cap.h` (add/add — both branches added new content)
- `scripts/gen_soc_caps.py` (content conflict)

Per the prior session's standing instruction ("If a merge fails with a conflict, STOP — don't auto-resolve"), the merge phase halted. The user's overnight failure policy (`Skip failed slice, keep going`) applied: skipped #17 + transitive descendants since they all carry #17's commits, kept moving to Phase 2.

PRs blocked by this:
- **#17** feat(backend): registry foundation + capability negotiation + CI gates (Slice 0)
- **#18** feat(adc): registry pilot (Slice 1) — retargeted to main, still conflicts (carries #17 commits)
- **#35** feat(periph): i2c/spi/uart registry migration (Slice 2A) — retargeted to main, same
- **#26, #19, #27, #28, #29** — all stacked, all carry #17

**Recommended user action**: resolve #17's 3 conflicts locally (the abi snapshot is regenerable via `python scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json`; cap.h + gen_soc_caps.py need a human read), push to `feat/backend-registry-foundation`, then admin-merge in the original order. Once #17 lands, the stacked PRs (#18, #35, #26, #19, #27, #28, #29) auto-cascade.

### Branch protection
Restored from `C:/Users/caner/AppData/Local/Temp/main-protection-put.json` (the older PUT-format backup — the fresh GET dump wasn't PUT-compatible, schema diff). `enforce_admins=true`, `required_status_checks.strict=true`, 12 required contexts. Verified live via `gh api repos/alplabai/alp-sdk/branches/main/protection`.

## Phase 2 — new slice PRs (ALL FIVE OPENED)

All five new PRs stack on existing in-flight branches because #17 isn't merged. When the user resolves the #17 conflict and the original stack lands, each new PR can be retargeted to main.

### #54 — Slice 2B: gpio + pwm + i2s + can (33 commits)
- Base: `feat/backend-registry-core-peripherals` (PR #35)
- 4 sequential subagents, one per peripheral. Strict serial (file-system conflict avoidance).
- Per-peripheral 8-commit chain mirroring SPI from #35 + 1 ABI snapshot commit.
- Deviations from sibling pattern:
  - **GPIO**: Zephyr `gpio_callback` lives in a backend-private sidecar (`alp_z_gpio_side_t[]`), not on the portable handle.
  - **PWM**: 9-entry vtable (drive-side + capture-side); capture pool sits parallel to the drive pool.
  - **I2S**: `k_mem_slab` + `slab_buf` moved into `alp_z_i2s_side_t` sidecar inside `src/backends/i2s/zephyr_drv.c`. **First slice to type `dev` as `void *`** in the ops vtable to partially mitigate issue #34 (`<zephyr/device.h>` leakage); CAN followed suit.
  - **CAN**: `started` flag preserved on portable handle. Per-handle `trampoline_key_t[][]` table replaces the legacy single-global callback table. SW fallback downgraded from loopback ring to stateless stub (loopback was ~80 LoC for one test case; NOSUPPORT contract is cleaner).
- ABI: 4 new capability getter entries (`alp_gpio_capabilities`, `alp_pwm_capabilities`, `alp_i2s_capabilities`, `alp_can_capabilities`).

### #55 — Slice 6: Storage + inline-AES (13 commits)
- Base: `feat/backend-registry-foundation` (PR #17)
- Replaces `src/zephyr/storage_stub.c` (71 lines, all NOSUPPORT) with:
  - `src/backends/storage/zephyr_flash.c` — real `flash_area` backend
  - `src/backends/storage/zephyr_littlefs.c` — real littlefs backend
  - `src/backends/storage/sw_fallback.c` — stateless stub
- Vendor extensions:
  - `include/alp/ext/alif/storage.h` + body — OSPI SecAES (NOSUPPORT until vendor pack)
  - `include/alp/ext/nxp/storage.h` + body — FlexSPI OTFAD (NOSUPPORT until vendor pack)
- `configure_inline_aes` input validation moved to the dispatcher's pre-backend gate.
- Yocto block-device backend deferred to slice #33 (per "NEVER touch src/yocto/*.c" guardrail).

### #56 — Slice 7: Power (8 commits, closes #22)
- Base: `feat/backend-registry-stub-slices` (PR #26)
- Real backend: `src/backends/power/zephyr_pm_policy.c` (343 lines) — uses `pm_policy_state_lock_get/put`. Registers at higher priority than #26's wildcard stub.
- Renesas vendor extension:
  - `include/alp/ext/renesas/power.h` declares `alp_renesas_power_supervisor_mode_set(handle, supervisor_mode)`
  - Body uses the existing `alp_z_v2n_supervisor_acquire/_release` singleton (same pattern as `src/backends/adc/gd32_bridge.c`) + `gd32g553_power_mode_set()` from the chip driver
  - **DID NOT touch `src/zephyr/v2n_power_mgmt.{c,h}`** (verified)
- Yocto `/sys/power/state` deferred to slice #33; documented in code.

### #57 — Slice 5: Camera + ISP (9 commits, closes #20 #21)
- Base: `feat/backend-registry-stub-slices` (PR #26)
- Real backends:
  - `src/backends/camera/zephyr_video.c` — Zephyr `drivers/video/` wrapper (priority 50)
  - `src/backends/camera/v2n_n44_isp.c` — V2N N44 ISP skeleton (priority 100, wins on `renesas:rzv2n:n44`)
- **First registry class with priority-stratified real backends.**
- Vendor extensions:
  - `include/alp/ext/renesas/camera.h` + body — fine-grained V2N N44 ISP knobs (3A windows, gain LUT, LSC LUT). MMIO pokes stubbed-ALP_OK with TBDs referencing Renesas Hardware User's Manual r01uh1003ej §18.x — real pokes land when the V2N N44 Zephyr SoC port drops.
  - `include/alp/ext/alif/camera.h` + body — Mali-C55 ISP for AEN E-series (NOSUPPORT until Alif HAL Mali-C55 pack)
- 5 TBD comments in code, all referencing concrete datasheet sections.

### #60 — Slice 3: Inference (15 commits)
- Base: `feat/backend-registry-foundation` (PR #17)
- **3 REAL backends + 2 NOT_IMPLEMENTED stubs + SW fallback**:

  | Backend | Status |
  |---|---|
  | `tflm` | REAL (port of `inference_tflm.cpp`) |
  | `ethos_u_aen` | REAL (TFLM + Ethos-U delegate via Alif HAL hook) |
  | `ethos_u_n93` | REAL (TFLM + Ethos-U delegate via NXP hook) |
  | `drpai_v2n_stub` | NOT_IMPLEMENTED — issue **#58** |
  | `deepx_dxm1_stub` | NOT_IMPLEMENTED — issue **#59** |
  | `sw_fallback` | NOSUPPORT floor |

- Vendor extensions (2 of spec's "3"):
  - `include/alp/ext/renesas/inference.h` + body — DRP-AI3 pipeline-stage knobs
  - `include/alp/ext/deepx/inference.h` + body — DX-M1 slot + tile management
  - **Alif inference ext DROPPED** on §3(a) audit — no distinct Alif HAL surface beyond what the Arm Ethos-U driver layer already covers. Documented in PR body; can revisit if the Alif HAL grows one.

- **Legacy retirement** (per memory feedback_no_legacy_compat — no active customers):
  - DELETED `src/zephyr/inference_{zephyr,tflm,drpai,ethosu_n93}.{c,cpp}` (870 LoC total)
  - DELETED 10 legacy Kconfigs: `ALP_SDK_INFERENCE_{TFLM, DRPAI, ETHOS_U, ETHOS_U_N93, ETHOS_U_U55, ETHOS_U_U65, ETHOS_U_U85, TFLM_NEON, TFLM_HELIUM, TFLM_REF}`
  - Replaced by per-backend ladder. No shims.

- Yocto-side bodies (`src/yocto/inference_yocto.c`, `src/yocto/inference_deepx.cpp`) untouched per the guardrail.

## Issues filed
- **#58** — `inference: real drpai_v2n backend body`
- **#59** — `inference: real deepx_dxm1 backend body (Zephyr-side)`
  (Yocto-side DEEPX body already exists at `src/yocto/inference_deepx.cpp`, owned by slice #33.)

## CI status

As of session end, **CI hasn't started on any of the 5 new PRs** (`gh pr checks` returns "no checks reported"). PRs #35 + earlier stacked PRs also show no checks — possibly because they target non-main branches and the workflow filters are main-only, OR CI is queued and hasn't fired yet.

**Worth checking on wake-up**: confirm `.github/workflows/*.yml` `on.pull_request.branches:` filters don't block non-main bases; if they do, retargeting the new PRs to main (after #17 resolves) is needed to get CI signal.

## Recommended wake-up sequence

1. **Resolve #17's 3 conflicts** (cap.h add/add + gen_soc_caps.py content + abi snapshot regenerate). Push to `feat/backend-registry-foundation`.
2. **Admin-merge the original stack** in dependency order: 17 → 18 → 26 → 19 → 27 → 28 → 29 → 35. Each merge should be clean once #17's resolution propagates.
3. **Retarget the new slice PRs to main** as their bases land:
   - #55, #60 (base #17) → main once #17 lands
   - #54 (base #35) → main once #35 lands
   - #56, #57 (base #26) → main once #26 lands
4. **Re-run CI** on each retargeted PR if `gh pr checks` still shows nothing.
5. **Review judgment calls flagged in PR bodies**:
   - PR #54: I2S/CAN ops use `void *` for `dev` (partial issue #34 mitigation) — keep, or revert for SPI/GPIO/PWM consistency?
   - PR #57: V2N N44 ISP stubbed-ALP_OK skeleton (MMIO pokes deferred) — accept, or block until real port?
   - PR #60: Alif inference ext dropped — accept, or add a placeholder header?

## Memory updates worth considering

Nothing new to save — all the patterns used (sequential subagents, ABI snapshot procedure, no-claude-footer, no-legacy-compat, no-invented-values) are already in `MEMORY.md`. The "admin-merge auto-close race" lesson from #16 → #36 might be worth a note in `feedback_admin_merge_for_branch_protection.md` if it bites again, but for now the workaround (recreate PR) is mechanical enough.

## Branch state at session end

- Working tree: clean
- Local checked-out branch: `main` (at `b1041db`, synced with origin)
- All 5 new slice branches pushed to origin with tracking
- Branch protection: restored (enforce_admins=true, 12 required contexts, strict mode)
