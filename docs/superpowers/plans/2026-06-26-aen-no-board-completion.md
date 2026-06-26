# AEN No-Board Completion Implementation Plan (A+B+C)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three board-independent completions: make the DT reserved-memory generator arch-aware (drop the AEN801 hand-adjust drift trap), add the grounded carrier i2c sensor nodes, and verify a correct Zephyr-SDK M55 firmware build.

**Architecture:** A adds a `linux_phys_addr_bits` SoC-JSON field that `emit_dts_reservations` uses to emit 1-cell (AArch32/AEN801) vs 2-cell (AArch64/V2N) reserved-memory, then regenerates the AEN801 dtsi from the fixed generator. B replaces the carrier DTS `&i2c2` TODO with device nodes transcribed from the board header. C installs the Zephyr SDK in WSL, rebuilds the M55 ELF correctly, and verifies the `alp_backends_*` iterable sections from the `.map` (ELF stays internal).

**Tech Stack:** Python 3.14 (`py -3.14`); JSON-Schema metadata; Yocto/bitbake + Alif `linux_alif` (WSL); Zephyr + west (M55 slice); clang-format-22.

## Global Constraints

- Never invent values — transcribe from `include/alp/boards/alp_e1m_evk.h` / SoC sources, else `TODO(...)`.
- `py -3.14` for all Python (default `python` is a stale 3.9). "Alp Lab" spelling; no `Co-Authored-By: Claude` footer.
- clang-format-22 for any C/H touched (verify with WSL `~/.local/bin/clang-format` 22.1.5).
- WSL bakes via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`; `MACHINE=e1m-aen801-a32`, `DISTRO=apss-tiny`, SP1 `BBMASK` + `BB_DANGLINGAPPENDS_WARNONLY=1`.
- No binaries in public git — the top-level `*.elf` guard stands; C's ELF stays internal/untracked.
- Branch `feat/aen-a32-yocto-bringup` (PR #264). WSL tree `~/alif-bsp-ref/alif_linux-apss-build-setup`.

---

### Task 1: Arch-aware DT reserved-memory cells (item A)

**Files:**
- Modify: `metadata/schemas/soc-spec-v1.schema.json` (add `linux_phys_addr_bits` property)
- Modify: `metadata/socs/alif/ensemble/e8.json` (add `"linux_phys_addr_bits": 32`)
- Modify: `metadata/socs/renesas/rzv2n/n44.json` (add `"linux_phys_addr_bits": 64`)
- Modify: `scripts/alp_orchestrate.py` (`emit_dts_reservations`)
- Modify: `tests/scripts/test_alp_orchestrate.py` (add the AArch32 cell-width test)
- Regenerate: `meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi`

**Interfaces:**
- Consumes: `BoardProject.soc_spec` (the loaded SoC JSON), `resolve_carve_outs(project)`.
- Produces: `emit_dts_reservations(project)` emitting `#address-cells/#size-cells = <cells>` where `cells = linux_phys_addr_bits // 32` (default 64 → 2).

- [ ] **Step 1: Add the schema property**

In `metadata/schemas/soc-spec-v1.schema.json`, under `properties`, add (match the file's existing property style/indentation):
```json
"linux_phys_addr_bits": {
  "type": "integer",
  "enum": [32, 64],
  "description": "Physical-address width of the SoC's Linux-running A-cluster; sets the reserved-memory DT #address-cells/#size-cells (bits/32). Optional; defaults to 64 when absent."
}
```

- [ ] **Step 2: Set the value on both SoCs**

Add a top-level `"linux_phys_addr_bits": 32,` to `metadata/socs/alif/ensemble/e8.json` (the A32 cluster is AArch32) and `"linux_phys_addr_bits": 64,` to `metadata/socs/renesas/rzv2n/n44.json` (the A55 cluster is AArch64). Place it near the other top-level scalars (e.g. after `soc_flash_mb`/`always_on_sram_kb`).

- [ ] **Step 3: Verify both SoC JSONs still validate**

Run:
```
py -3.14 -c "import json,jsonschema; s=json.load(open('metadata/schemas/soc-spec-v1.schema.json')); [jsonschema.validate(json.load(open(f)),s) for f in ['metadata/socs/alif/ensemble/e8.json','metadata/socs/renesas/rzv2n/n44.json']]; print('both validate')"
```
Expected: `both validate`. (If the repo has a dedicated SoC-schema test, run it instead: `py -3.14 -m pytest tests/scripts -k soc -q`.)

- [ ] **Step 4: Write the failing AArch32 cell-width test**

In `tests/scripts/test_alp_orchestrate.py`, after `test_emit_dts_reservations_shape`, add (the `AEN801_M55_IPC` fixture already exists in the file and resolves a carveout):
```python
def test_emit_dts_reservations_aarch32_cells(tmp_path: Path) -> None:
    """E1M-AEN801 is AArch32 (linux_phys_addr_bits=32) so the
    reserved-memory node must use single-cell addressing with a 2-word
    reg, matching the base ensemble-ex.dtsi #address-cells = <1>."""
    path = _write_board(tmp_path, AEN801_M55_IPC)
    out = emit_dts_reservations(load_board_yaml(path))
    assert "#address-cells = <1>;" in out
    assert "#size-cells = <1>;" in out
    # 1-cell reg: base + size as two 32-bit words (base 0x023f0000, 64 KiB).
    assert "reg = <0x023f0000 0x00010000>;" in out
    # The 2-cell 4-word form must NOT appear for this AArch32 target.
    assert "#address-cells = <2>;" not in out
```

- [ ] **Step 5: Run it — verify it FAILS**

Run: `py -3.14 -m pytest tests/scripts/test_alp_orchestrate.py::test_emit_dts_reservations_aarch32_cells -q`
Expected: FAIL (the generator still emits `<2>` + 4-word reg).

- [ ] **Step 6: Make the generator arch-aware**

In `scripts/alp_orchestrate.py::emit_dts_reservations`, replace the hardcoded header + cell lines and the reg formatting. The function currently starts:
```python
def emit_dts_reservations(project: BoardProject) -> str:
    """Generate dts-reservations.dtsi per spec §6.2."""
    carve_outs = resolve_carve_outs(project)
    lines: list[str] = [
```
Change the body to compute cells and emit accordingly (remove the old `TODO(arch-aware-cells)` comment block added in SP3 Task 5):
```python
def emit_dts_reservations(project: BoardProject) -> str:
    """Generate dts-reservations.dtsi per spec §6.2."""
    carve_outs = resolve_carve_outs(project)
    # Reserved-memory cell width follows the SoC's Linux A-cluster
    # physical-address width: 32-bit AArch32 -> 1 cell, 64-bit AArch64
    # -> 2 cells.  Default 64 preserves behavior for SoCs that have not
    # declared linux_phys_addr_bits.
    bits = int(project.soc_spec.get("linux_phys_addr_bits", 64))
    cells = bits // 32
    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_orchestrate.py -- do not edit.",
        " * Regenerate after changes to board.yaml `ipc:` or the SoM's",
        " * memory_map block.  #include this file from your kernel /",
        " * Zephyr DT.",
        " */",
        "",
        "/ {",
        "    reserved-memory {",
        f"        #address-cells = <{cells}>;",
        f"        #size-cells = <{cells}>;",
        "",
    ]
```
Then in the per-carveout loop, replace the base/size split + the `reg = ...` line with a cell-aware form (keep the 2-cell output BYTE-IDENTICAL to today so V2N is unchanged):
```python
            if cells == 1:
                reg = f"0x{c.base:08x} 0x{c.size:08x}"
            else:
                base_hi = (c.base >> 32) & 0xFFFFFFFF
                base_lo = c.base & 0xFFFFFFFF
                size_hi = (c.size >> 32) & 0xFFFFFFFF
                size_lo = c.size & 0xFFFFFFFF
                reg = (f"0x{base_hi:x} 0x{base_lo:08x} "
                       f"0x{size_hi:x} 0x{size_lo:08x}")
            lines.append(
                f"        {c.name}: {c.name}@{c.base:x} {{")
            lines.append(
                '            compatible = "shared-dma-pool";')
            lines.append(f"            reg = <{reg}>;")
            lines.append("            no-map;")
            lines.append(f'            label = "{c.name}";')
            lines.append("        };")
            lines.append("")
```

- [ ] **Step 7: Run the new test + the V2N shape test — both PASS**

Run: `py -3.14 -m pytest tests/scripts/test_alp_orchestrate.py -k "dts_reservations" -q`
Expected: PASS — `test_emit_dts_reservations_aarch32_cells` (AEN801 `<1>`) and `test_emit_dts_reservations_shape` (V2N still `<2>`).

- [ ] **Step 8: Run the full orchestrate suite — no regressions**

Run: `py -3.14 -m pytest tests/scripts/test_alp_orchestrate.py -q`
Expected: all pass (any pre-existing Windows `bmaptool` skip/fail is unrelated — note it).

- [ ] **Step 9: Regenerate the AEN801 dtsi from the fixed generator (drops the hand-adjust)**

Run:
```
py -3.14 scripts/alp_orchestrate.py --input examples/multicore/rpmsg-aen/board.yaml --emit dts-reservations > meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi
```
Then confirm the regen is byte-stable (a second emit equals the file):
```
py -3.14 scripts/alp_orchestrate.py --input examples/multicore/rpmsg-aen/board.yaml --emit dts-reservations | diff - meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi && echo "dtsi is generator-faithful"
```
Expected: `dtsi is generator-faithful`. The new file has the clean "Auto-generated -- do not edit" header (no hand-adjust note) and `reg = <0x023c0000 0x00040000>` (1-cell).

- [ ] **Step 10: Commit**

```bash
git add metadata/schemas/soc-spec-v1.schema.json metadata/socs/alif/ensemble/e8.json metadata/socs/renesas/rzv2n/n44.json scripts/alp_orchestrate.py tests/scripts/test_alp_orchestrate.py meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi
git commit -m "feat(orchestrate): arch-aware DT reserved-memory cells (linux_phys_addr_bits); regen AEN801 dtsi"
```

---

### Task 2: Carrier DTS on-bus i2c sensor nodes (item B)

**Files:**
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` (the `&i2c2` block)

**Interfaces:**
- Consumes: the SP1 `&i2c2` controller node (already `#address-cells = <1>; #size-cells = <0>;`).
- Produces: six device child nodes at their board-header addresses.

- [ ] **Step 1: Replace the `&i2c2` TODO with the device nodes**

In `e1m-aen801-evk.dts`, the `&i2c2` node has a `TODO(e1m-evk-hw)` comment block listing the on-bus devices. Replace that comment (keep the controller properties — `#address-cells`, `#size-cells`, `pinctrl-*`, `status`) with these child nodes (addresses transcribed from `include/alp/boards/alp_e1m_evk.h`: `EVK_I2C_ADDR_BMI323`=0x68, `EVK_I2C_ADDR_ICM42670`=0x69, `EVK_I2C_ADDR_BMP581`=0x47, `EVK_I2C_ADDR_INA236` 0x40-0x42 / 0x49-0x4B, `EVK_I2C_ADDR_TCAL9538`=0x72):
```dts
	/*
	 * On-bus EVK-carrier devices (addresses from
	 * include/alp/boards/alp_e1m_evk.h).  Nodes whose Alif-kernel
	 * driver is absent simply don't bind -- harmless; the SP2
	 * userspace alp_i2c (/dev/i2c-N) path is unaffected.  Confirm the
	 * exact `compatible` strings the booted kernel binds at bench.
	 */
	imu_bmi323: imu@68 {
		compatible = "bosch,bmi323";
		reg = <0x68>;
	};

	imu_icm42670: imu@69 {
		compatible = "invensense,icm42670p";
		reg = <0x69>;
	};

	baro_bmp581: barometer@47 {
		compatible = "bosch,bmp581";
		reg = <0x47>;
	};

	pmon_3v3: power-monitor@40 {
		compatible = "ti,ina236";
		reg = <0x40>;
	};
	pmon_1v8: power-monitor@41 {
		compatible = "ti,ina236";
		reg = <0x41>;
	};
	pmon_vddio: power-monitor@42 {
		compatible = "ti,ina236";
		reg = <0x42>;
	};
	pmon_vbat: power-monitor@43 {
		compatible = "ti,ina236";
		reg = <0x43>;
	};
	pmon_vcc: power-monitor@44 {
		compatible = "ti,ina236";
		reg = <0x44>;
	};
	pmon_vsys: power-monitor@45 {
		compatible = "ti,ina236";
		reg = <0x45>;
	};

	ioexp_tcal9538: gpio@72 {
		compatible = "ti,tca9538";
		reg = <0x72>;
		gpio-controller;
		#gpio-cells = <2>;
	};
```
Match the file's existing tab indentation. (The INA236 rail labels are descriptive node names only — if the board header documents specific rail→address bindings, use those names; otherwise these generic `pmon_*` labels are acceptable since the node identity is `power-monitor@<addr>`.)

- [ ] **Step 2: Compile the dtb + decompile-verify (WSL)**

Write `scratchpad/build_b_dtb.sh` (source the env, `bitbake -c cleansstate linux-alif`, `bitbake -c compile linux-alif`, then `dtc -I dtb -O dts` the produced `e1m-aen801-evk.dtb`) and run it via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/build_b_dtb.sh`. Grep the decompile:
```
... | grep -iE "bosch,bmi323|invensense,icm42670|bosch,bmp581|ti,ina236|ti,tca9538|@6[89]|@4[0-9a-b]|@72"
```
Expected: the six device nodes appear under the i2c2 controller at 0x68/0x69/0x47/0x40-0x42+0x49-0x4B/0x72. This is a LONG kernel rebuild — let it finish. If the dtc decompile errors on a node, fix the DTS and rebuild.

- [ ] **Step 3: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts
git commit -m "dts(aen): add the EVK carrier on-bus i2c sensor nodes (bmi323/icm42670/bmp581/ina236/tcal9538)"
```

---

### Task 3: M55-HP firmware Zephyr-SDK rebuild + iterable-section verify (item C)

**Files:**
- Modify: `meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/aen-m55-hp-fw_0.6.bb` (comment: confirmed Zephyr-SDK build)
- Modify: `examples/multicore/rpmsg-aen/README.md` (the build note)
- Modify: `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` (the `.map` evidence)
- (No ELF is committed — it stays internal/untracked.)

**Interfaces:**
- Consumes: the M55-HP slice `examples/multicore/rpmsg-aen/m55_hp/`, the Zephyr board `alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp`.
- Produces: the `.map` evidence that `__alp_backends_start`..`__alp_backends_end` bracket the backend entries; a corrected build note.

- [ ] **Step 1: Install the Zephyr SDK in WSL**

Probe + install (`west` is at `~/.local/bin/west`; SDK is absent). Write `scratchpad/install_zephyr_sdk.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
which west
west --version
# Install the SDK non-interactively if west supports it; else the upstream installer.
west sdk install 2>&1 | tail -20 || {
  echo "west sdk install unavailable; report BLOCKED with this output"; exit 1; }
echo "ZEPHYR_SDK_INSTALL_DIR after install:"; ls ~/zephyr-sdk* 2>/dev/null || true
```
Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/install_zephyr_sdk.sh`.
**If the SDK cannot be installed** (no `west sdk` subcommand, network/disk limits): STOP, report **BLOCKED** with the exact output — do NOT fake it; C defers to a Zephyr-SDK-equipped host and the gnuarmemb caveat stays documented.

- [ ] **Step 2: Build the M55-HP slice with the Zephyr toolchain + keep the map**

Write `scratchpad/build_m55_zephyr.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk
export ALP_SDK_ROOT=$PWD
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
west build -b alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp \
  examples/multicore/rpmsg-aen/m55_hp -d /tmp/m55_zephyr -p always 2>&1 | tee /mnt/c/Users/caner/AppData/Local/Temp/claude/m55_zephyr_build.log
file /tmp/m55_zephyr/zephyr/zephyr.elf
```
Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/build_m55_zephyr.sh`.
Expected: a Cortex-M55 ELF, and the build log shows **no orphan-section warning** for `alp_backends_*` (the warning gnuarmemb produced should be gone with the Zephyr linker script).

- [ ] **Step 3: Verify the iterable-section brackets from the map**

Run (the Zephyr build emits `zephyr.map`):
```
MSYS_NO_PATHCONV=1 wsl bash -lc "grep -nE '__alp_backends_(start|end)|alp_backends' /tmp/m55_zephyr/zephyr/zephyr.map | head -40"
```
Expected: `__alp_backends_start` and `__alp_backends_end` appear with the backend entries BETWEEN them (not orphaned at the image tail). Capture the relevant `.map` lines (≤30) for the report.

- [ ] **Step 4: Stage the internal ELF for local bakes (untracked) + record evidence**

Copy the verified ELF to the recipe files dir (it stays git-ignored by `*.elf`):
```
cp /tmp/m55_zephyr/zephyr/zephyr.elf meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/files/m55_hp.elf   # run in WSL
git check-ignore meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/files/m55_hp.elf   # must print the path (ignored)
```
Append a `## Zephyr-SDK M55 build (verified)` section to `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` with the `file` output, the no-orphan-warning confirmation, and the `.map` bracket lines.

- [ ] **Step 5: Update the recipe + README build note**

In `aen-m55-hp-fw_0.6.bb`, change the comment that warns about the gnuarmemb orphan-section risk to state the **verified** Zephyr-SDK build is the supported path (keep the "build out-of-tree / internal" framing). Mirror the note in `examples/multicore/rpmsg-aen/README.md`. Run `py -3.14 scripts/check_doc_drift.py` (expect OK).

- [ ] **Step 6: Commit (docs/recipe only — NO elf)**

```bash
git add meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/aen-m55-hp-fw_0.6.bb examples/multicore/rpmsg-aen/README.md docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md
git status --porcelain | grep -i "\.elf" && echo "ERROR: elf staged — unstage it" || true
git commit -m "docs(aen): verified Zephyr-SDK M55 firmware build (alp_backends_* iterable sections placed)"
```

---

## Self-Review

**Spec coverage:**
- A (schema field + metadata + generator + regen + tests) → Task A. ✅
- B (carrier i2c nodes from board-header addrs) → Task B. ✅
- C (Zephyr SDK install + rebuild + `.map` verify + internal ELF + doc) → Task C, with the BLOCKED-degrade path in Step 1. ✅
- Global constraints (no invented values, no committed binary, clang-format-22, py-3.14) → Global Constraints + each task's commit guards. ✅

**Placeholder scan:** No "TBD"/"implement later" in requirements. The DTS `TODO(...)` references are the EXISTING placeholders being REPLACED (Task B) or kept where genuinely board-gated (the `compatible`-confirm note). C's BLOCKED path is an explicit honest degrade, not a gap.

**Type/name consistency:** `linux_phys_addr_bits` identical across schema/e8/n44/generator/test. `cells = bits // 32`. The 2-cell reg format in Task A Step 6 is byte-identical to the current generator (V2N unchanged — asserted by the untouched `test_emit_dts_reservations_shape`). Firmware path `/lib/firmware/alp/E1M-AEN801/m55_hp.elf` + the untracked `files/m55_hp.elf` consistent with SP3. The AEN801 carveout base `0x023f0000` (64 KiB test fixture) vs `0x023c0000` (256 KiB real board.yaml) are different carve sizes — both correct for their fixture.
