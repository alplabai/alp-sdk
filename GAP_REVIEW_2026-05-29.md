# alp-sdk Implementation-Gap & Consistency Review — 2026-05-29

Recovered from the cached `alp-sdk-gap-review` workflow run (`wf_81216728-5ce`): 25 area
reviews → 171 adversarial verifications → **147 confirmed issues** (24 rejected by
verification). Severity reflects the verifier's corrected rating.

| Severity | Count |
|---|---|
| Critical | 0 |
| High | 18 |
| Medium | 49 |
| Low | 80 |

---

## Executive summary

- **DEEPX backend name was never fully canonicalised.** `DEEPX_DX` / `deepx_dx` /
  `dx-com` / `dx-compiler` / `deepx-dx-m1` survive in the Yocto `dx-rt` recipe, `zephyr/Kconfig`,
  `include/alp/soc_caps.h`, `include/alp/inference.h`, `src/yocto/inference_yocto.c`,
  `src/inference_dispatch.c`, four `metadata` files, and several example READMEs. Canonical is
  `DEEPX_DXM1` / `dxcom`. This is the single most repeated drift in the tree (~15 findings).
- **The AI/NPU pipeline is advertised as operational but the device backends are stubs.**
  README + architecture present `.alpmodel` → DRP-AI3/DEEPX dispatch as working, but the device
  backends are `NOSUPPORT` and compiler adapters raise `NotImplementedError`. Multiple AI
  examples (`ai-anomaly`, `ai-object-detection`, `ai-camera-viewer`, `edgeai-vision-aen`,
  `iot-connected-camera`) emit hardcoded/zeroed inference results from stub models.
- **The Yocto layer cannot build the V2M/DEEPX or i.MX93 paths as documented.** Fabricated
  all-zero `SRCREV`/`sha256`/`LIC_FILES_CHKSUM`, a `require` of a non-existent
  `imx93-base.inc`, `KERNEL_DEVICETREE` pointing at a `e1m-x-evk-v2m.dts` no patch produces, and
  a wholly broken `bring-up-imx93.md` (kirkstone + kernel 6.6 + `meta-bsp`/`meta-ml` +
  `DISTRO="alp-distro"`, none of which exist).
- **The examples reorg left stale flat paths across the docs.** `getting-started.md`,
  `local-ci.md`, `cross-platform-setup.md`, and `tutorials/16` all build example paths that no
  longer exist.
- **Single-source-of-truth violations leak SoM-internal routing into SoM-agnostic files.**
  The CC3501E mediator is named in the carrier `e1m-evk.yaml`; `gd32g553.yaml` (a chip file)
  embeds carrier-level Renesas/GD32 pin routing that duplicates the V2N TSVs; carrier DT patches
  + bbappend leak PHY part numbers, MDIO straps and SoC pins.
- **Nine chip drivers ship with no metadata manifest and there is no gate enforcing the
  correspondence** (`cc3501e`, `icm42670`, `tas2563`, … ; conversely `pca9451a` is a manifest
  with no driver).
- **Personal local paths are committed** in `tools/altium/export_e1m_pinmap.pas`,
  `tools/altium/README.md`, `docs/local-ci.md`, and the untracked `_review_filelist.txt` scratch
  file — a leak-hygiene regression.
- **Several "documentation" examples contradict their own code** (drone-hud Madgwick claim vs raw
  gyro integrator; i2s-tone "sine" vs triangle; mproc-mailbox APIs that don't exist; UART0 opened
  twice for two roles).

---

## Issues by severity

### High (18)

- [ ] **`require imx93-base.inc` target missing** `meta-alp-sdk/conf/machine/e1m-nx9101-a55.conf:11` — file doesn't exist; machine conf can't parse. Create the include or inline it.
- [ ] **alp-sdk / alp-perception recipes pin all-zero `SRCREV`** `meta-alp-sdk/recipes-core/alp-sdk/alp-sdk_0.5.bb:18`, `alp-perception_0.5.bb:21` — fabricated rev can't resolve; pin a real SHA or `AUTOREV`.
- [ ] **CC3501E mediator named in SoM-agnostic carrier YAML** `metadata/boards/e1m-evk.yaml` (E1M_SPI1 L146, E1M_GPIO_IO15 L111) — move SoM-internal dispatch to the SoM side (known `routes_via` violation class).
- [ ] **Carrier DT patches + bbappend leak SoM schematic detail** `meta-alp-sdk/recipes-kernel/linux/linux-renesas_%.bbappend` (hdr L9-15; patches 0008/0009/0011/0012/0013) — PHY part/ID, MDIO straps, SoC pins, unrouted RIIC. Sanitise or move to internal.
- [ ] **Two V2N inference examples use incompatible `alp_inference_config_t` field names** `examples/v2n/v2n-m1-deepx-inference/src/main.c:86-93` vs `v2n-m1-ros-perception/src/deepx_dispatch.cpp:32-43` — one won't compile against the real struct.
- [ ] **`audio-loopback` never runs `alp_project.py`** `examples/audio/audio-loopback/CMakeLists.txt` — `board.yaml` (i2s→`CONFIG_I2S=y`) silently ignored; the example builds without its peripheral.
- [ ] **`CONFIG_DUMMY_DISPLAY=y` in board-agnostic `prj.conf`** `examples/display/{lvgl-widgets-demo:33-34, drone-hud:21-22, lvgl-benchmark:17-18, lvgl-music-player:17-18}` — real-silicon build also compiles the dummy display. Move to native_sim overlay.
- [ ] **README/main.c point at non-existent SKU `E1M-V2N201`** `examples/ai/ai-anomaly-detection-vibration/README.md:~34`, `src/main.c:43-46` — "flip som.sku to E1M-V2N201" — that SKU doesn't exist.
- [ ] **mproc-mailbox README documents non-existent APIs** `examples/multicore/mproc-mailbox/README.md:12-18` — `alp_shmem_write_at` / `alp_mbox_recv` are absent from `<alp/mproc.h>`.
- [ ] **drone-autopilot opens `E1M_UART0` twice** `mavlink.c:329-331` (GCS) vs `autopilot.c:121-123` (GNSS) — port collision.
- [ ] **drone-autopilot RC loop never calls its SBUS decoder** `examples/peripheral-io/drone-autopilot/src/main.c:~233-253` — feeds synthetic neutral sticks via `if (true)` stub.
- [ ] **Missing chip manifests** `metadata/chips/{cc3501e,icm42670}.yaml` absent though full driver + Kconfig (+Yocto for ICM) ship. (See systemic item.)
- [ ] **PWM6+PWM7 Renesas route claimed in protocol doc** `docs/gd32-bridge-protocol.md §3.2:276-278` — contradicts GD32-only canonical metadata (Renesas drives zero E1M PWMs).
- [ ] **Personal local path in tracked Altium script** `tools/altium/export_e1m_pinmap.pas:24,64` — strip the `C:\Users\…` default.
- [ ] **V2M101 SoM example uses E1M (not E1M-X) board preset** `examples/v2n/v2n-m1-deepx-inference/board.yaml:9-11` — wrong family preset.
- [ ] **Contradictory native_sim-on-Windows guidance** `docs/local-ci.md:104-108/164-168` vs `cross-platform-setup.md:42,338-345` vs `getting-started.md:162-165`.
- [ ] **Run-more-examples loop uses post-reorg flat paths** `docs/getting-started.md:232-238`. (See systemic flat-path item.)

### Medium (49)

**Yocto / Linux image**
- [ ] **`dx-rt` recipe: all-zero LICENSE checksum, placeholder src/sha256, no LICENSE file — yet wired into V2M `IMAGE_INSTALL`** `meta-alp-sdk/recipes-deepx/dx-rt/dx-rt_2.4.bb:17,23-24`.
- [ ] **V2M machine confs point `KERNEL_DEVICETREE` at `e1m-x-evk-v2m.dts` no patch produces** `e1m-v2m101-a55.conf:45`, `e1m-v2m102-a55.conf:~38`, bbappend:36 — DT patches are V2N101-only.
- [ ] **`bring-up-imx93.md` sets `DISTRO="alp-distro"` (no such distro)** `docs/bring-up-imx93.md §3:103`.
- [ ] **`bring-up-imx93.md` omits meta-freescale, uses non-existent `meta-bsp`/`meta-ml`** `§3:90,97-98`.
- [ ] **imx93 builds `core-image-minimal` but payloads ship only in `alp-image-edge`** `docs/bring-up-imx93.md §3:106-114`.
- [ ] **`linux-renesas_%.bbappend` %-wildcard with DTS patches pinned to one kernel SHA, no SRCREV constraint** `bbappend:21-24`.
- [ ] **`alp-sdk` runtime recipe a full release behind (`0.5` vs co-deps `0.6`)** `alp-sdk_0.5.bb:21`.

**Metadata / single-source-of-truth**
- [ ] **TAS2563 driver+Kconfig+Yocto but no `metadata/chips/tas2563.yaml`**.
- [ ] **No gate enforces driver↔manifest correspondence** `metadata/chips/` vs `chips/` — 9 drivers lack a manifest; `pca9451a` is a manifest with no driver.
- [ ] **GD32G553 core clock disagrees: 216 MHz (chip meta) vs 240 MHz (counter_clock_hz + doc)** `metadata/chips/gd32g553.yaml:13,78`, `docs/gd32-bridge-protocol.md §3.8:343`.
- [ ] **RZ/V2N `memory_regions` hard-codes 4 GB `ddr_main`** `metadata/socs/renesas/rzv2n/n44.json:177` — can't represent a memory-variant-populated family on one silicon variant.
- [ ] **V2N102/V2M102 "larger memory" delta lives only in comments + unconsumed `memory:` block** `metadata/e1m_modules/E1M-V2N102.yaml:51-52`.
- [ ] **Alif E8 `pending_alif_datasheet:true` contradicts its released-v1.0 datasheet block** `metadata/socs/alif/ensemble/e8.json:8 vs 9-14,240`.
- [ ] **DA9292 secondary-PMIC I2C address drifts: doc 0x1C vs metadata 0x1E** `docs/soms/v2n.md:25`.
- [ ] **Silicon part number drift: `r9a09g056n48` (docs) vs `N44` (metadata)** `docs/e1m-x-v2n-sdk-integration.md:8`, `docs/errata-e1m-x-v2n.md:1`.
- [ ] **SWD clock name drift: `GD32_SWDCLK` vs canonical `GD32_SWCLK`** `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv:60` vs the rest.
- [ ] **V2M101 capability key `deepx_dx: true` (de-canonicalised)** `E1M-V2M101.yaml:63`. (See DEEPX systemic item.)

**Examples — build wiring & coverage**
- [ ] **audio-noise-suppression / audio-wake-word write `alp.conf` to build root, not `generated/`** — violates GLOB-escape convention.
- [ ] **Display examples emit `alp.conf` flat + overwrite `OVERLAY_CONFIG`** `examples/display/*/CMakeLists.txt:13,20` — diverges from canonical `generated/alp.conf` + `list(APPEND)`.
- [ ] **audio-noise-suppression feeds 240-sample blocks into a 256-pt FFT chain** `src/main.c:~282-284` — `alp_dsp_chain_apply_bins` fails every block (`ALP_ERR_OUT_OF_RANGE`).
- [ ] **No native_sim.conf overlays despite real-silicon-only Kconfigs** `v2n-emmc-block-stat/prj.conf`, `v2n-xspi-flash-readwrite/prj.conf`, `audio-noise-suppression`.
- [ ] **`spi-loopback`/`spi-master` locally `#define ALP_SPI_NO_CS`** the SDK doesn't export — `spi-loopback/src/main.c:25`, `spi-master/src/main.c:59`.
- [ ] **mproc-mailbox native_sim testcase omits `CONFIG_ALP_SDK_MPROC`** `testcase.yaml:19-25` — unlike its AEN sibling.
- [ ] **`dac-waveform` lacks a native_sim overlay** despite a native_sim build_only testcase.
- [ ] **`v2n-m1-ros-perception` has no prj.conf/testcase.yaml** — outside the twister native_sim gate.
- [ ] **Unused chip-specific/pinout includes in portable example sources** `ai-camera-viewer` (`<alp/chips/ov5640.h>`, `<alp/e1m_pinout.h>`).

**Examples — code vs docs**
- [ ] **drone-hud claims Madgwick "stable quaternion" but integrates raw gyro Euler, discards accel, drifts** `src/sensors.c:89-108`.
- [ ] **lvgl-music-player declares full audio chain its code never builds (I2S is a TODO)** `src/main.c:62-67`.
- [ ] **power-managed-sensor: README says edit `RTC_TICK_S` macro that doesn't exist; main.c is pure printf framing yet header claims real `open()`** `README.md:136-140`, `src/main.c:19-23`.
- [ ] **ai-anomaly demo emits `score=0.0000` on every path (1-byte stub model + zero-fill)** `src/main.c:108,137-142`.

**AI pipeline naming/drift**
- [ ] **`dx_compiler` / `dx-com` / `dx-compiler` instead of `dxcom`** `examples/camera-vision/ai-object-detection-realtime/README.md:78`, `v2n-m1-deepx-inference/README.md:71-72`.
- [ ] **Tutorial 16 builds non-existent `examples/inference-mobilenet`** `docs/tutorials/16-inference-mobilenet.md:193`.

**Docs — architecture / portability / setup / security**
- [ ] **README "Supported hardware" says i.MX93 = Yocto-only, contradicting the heterogeneous + portability tables** `README.md:474 vs 222`, `docs/portability-matrix.md:42,62`.
- [ ] **`architecture.md` repo-layout omits `vendors/nxp-imx93`, `vendors/deepx-dxm1`, `src/backends`** `:95-97,80-84`.
- [ ] **`architecture.md` describes obsolete `src/<os>/i2c.c → vendors/<som>/i2c.c` dispatch** `:327-356`.
- [ ] **local-ci.md / cross-platform-setup.md verification builds use non-existent flat example paths** `local-ci.md:161`, `cross-platform-setup.md:505-509`.
- [ ] **board-id.md & v2n.md cite an ADC2_CH7 8-bin divider table absent from `v2n/hw-revisions.yaml`** `docs/board-id.md:119-120,162-167`, `docs/soms/v2n.md:79-81`.
- [ ] **CC3501E brownout trip voltage contradicts across the two CC3501E docs (~1.3/1.6/1.71 V)** `cc3501e-bridge.md:66` vs `cc3501e-integration-plan.md §3.9.1:680-687`.
- [ ] **`from-cc3501e.tsv` lacks the proxied GPIO pads the bridge doc attributes to it** `docs/cc3501e-bridge.md:12-16,176`.
- [ ] **Trust-model: three docs name three different signature anchors for the same OTA artifact check** `ota-device-contract.md:36-38` vs `ota.md:89-90` vs `secure-boot.md:27-28`.
- [ ] **`local-ci.md` hardcodes the maintainer's Documents/GitHub layout and leads with the slow /mnt/c anti-pattern** `:65,73,84,133,143-156`.
- [ ] **Personal local path in tracked Altium README** `tools/altium/README.md:24`.

**Bridges / boards**
- [ ] **All four display examples route ST7789 over `EVK_SPI_BUS_ARDUINO`, which EVK metadata says terminates on the CC3501E, not the SoC** `examples/display/*/board.yaml`.
- [ ] **AEN presets reference chip slug `cc3501e` with no backing manifest** `E1M-AEN601.yaml:16,109`.

### Low (80)

80 lower-severity items — mostly naming drift, stale doc lines, and minor convention nits.
Dominant clusters (full per-item detail in `_confirmed_findings.json`):

- [ ] **DEEPX `DEEPX_DX` / `deepx_dx` / `deepx-dx-m1` residue** across `dx-rt_2.4.bb` (DESCRIPTION/SRC/LIC), `zephyr/Kconfig:52`, `include/alp/soc_caps.h:255`, `include/alp/inference.h:14-39`, `src/yocto/inference_yocto.c:15-16,117`, `src/inference_dispatch.c:26`, `metadata/socs/deepx/dx/m1.json:26,75`, `E1M-V2M101/102.yaml`, several example READMEs. → fold into one canonicalisation sweep.
- [ ] **AI examples return hardcoded/constant inference output**: `ai-camera-viewer` (frame copied, never inferred; hardcoded box), `ai-object-detection-realtime` decode returns constant box, `edgeai-vision-aen` 16-byte fake VELA blob, `iot-connected-camera` every stage print-and-skip.
- [ ] **README/architecture present `.alpmodel`→DRP-AI3/DEEPX dispatch as operational** while device backends are NOSUPPORT and adapters raise NotImplementedError `README.md:315,384-386`.
- [ ] **Stale per-version "lands in v0.x" schedules baked into shipped surfaces** `include/alp/inference.h:14-23`, `metadata/socs/deepx/dx/m1.json:75`, `docs/architecture.md:312-376`.
- [ ] **getting-started/firmware-quickstart drift**: `alp.py` vs real `alp_build.py` (`:143`), "11 minimal apps" miscount (`:413-415`), i.MX93 classed as E1M-X family.
- [ ] **imx93 bring-up Yocto release impossible (kirkstone + kernel 6.6 vs declared Scarthgap)** `docs/bring-up-imx93.md §2:70-72,§3:88-90`.
- [ ] **README/architecture ISP pipeline stage shown for an E7 SoM that has no ISP** `edgeai-vision-aen/README.md:7-11`, `docs/pipeline.md:13-19`.
- [ ] **Server-side OTA design / Mender server responsibilities described in this repo** `docs/ota.md:44-49,81-85`; Mender signing key as in-repo `keys/` path `ota.md:45`.
- [ ] **Leaks**: SoM-internal Renesas pins in `docs/soms/v2n-m1.md:34-36`; GPIO37 strap detail in `cc3501e-bridge.md:84-95`; `gd32g553.yaml` embeds V2N carrier routing; untracked `_review_filelist.txt` with home paths.
- [ ] **`capabilities:` block in SoM presets is schema-unvalidated** `som-preset-v1.schema.json` — typo/non-canonical keys pass silently (this is *why* `deepx_dx` slipped through).
- [ ] **i2c-slave / spi-slave ship as runnable examples but are NOSUPPORT stubs** whose `alp_*_slave_*` API exists in no header.
- [ ] **Stub/no-op example paths**: `v2n-gd32-swd-flash` readback (`:158-164`), `iot-fleet-ota` buffer-aliasing happy path (`:313-340`).
- [ ] **README↔code mismatches**: i2s-tone "sine" vs triangle, mproc-mailbox expected-output vs printf, timer-periodic-interrupt expected line, heterogeneous-offload `[m55_sm]` vs `[m33_sm]`.
- [ ] **Alif NPU presence declared in two disagreeing places (capability count vs per-variant optional_features)** `e3.json`, `e8.json`; E6 empty `peripherals:{}` without `pending_reference_manual_ingestion`.
- [ ] **Misc convention nits**: counter-alarm raw magic 0 + printk-in-callback, drone-hud raw INA236 0x40 + non-volatile shared telem, dac-waveform `dac` expressed as `adc` token (no `dac` schema token), inconsistent alp.conf overlay paths/mechanisms across connectivity examples.

*(The remaining low items are individual stale-doc / naming lines — see the JSON.)*

---

## Implementation gaps (genuinely unbuilt / stubbed, not merely buggy)

1. **DRP-AI3 & DEEPX device backends** — NOSUPPORT stubs; compiler adapters raise
   `NotImplementedError`. Docs imply operational dispatch.
2. **`.alpmodel` end-to-end inference in examples** — every AI example (`ai-anomaly`,
   `ai-object-detection`, `ai-camera-viewer`, `edgeai-vision-aen`, `iot-connected-camera`) runs a
   stub model / constant output.
3. **V2M (DEEPX) Yocto build path** — no `e1m-x-evk-v2m.dts` patch; `dx-rt` recipe is placeholder;
   carrier DT patches are V2N101-only.
4. **i.MX93 Yocto path** — `bring-up-imx93.md` is non-functional end to end; `imx93-base.inc`
   missing.
5. **Chip manifests for 9 drivers** (`cc3501e`, `icm42670`, `tas2563`, …) and the gate that would
   catch them.
6. **`i2c-slave` / `spi-slave` peripheral-mode API** — examples exist; the API does not.
7. **Portable signing API** (`alp_sign_*`) — `v2n-secure-element-sign` hand-rolls raw OPTIGA APDUs.
8. **Production board_id ADC channel** still TBD on `e1m-evk.yaml` while marked production/complete.
9. **drone-autopilot SBUS RC path** — decoder implemented but bypassed by an `if (true)` stub.

---

## Cross-cutting / systemic

- **Naming canonicalisation incomplete** — `DEEPX_DXM1` / `dxcom` / `DRP-AI3` not enforced; ~15
  findings. Root enabler: the `capabilities:` block has no schema, so bad keys never fail CI.
- **Single-source-of-truth violations** — SoM-internal routing leaks into carrier YAML
  (`e1m-evk.yaml`), chip files (`gd32g553.yaml`), and DT patches; memory deltas live in comments
  instead of the SoC-variant JSON.
- **Examples-reorg path drift** — flat `examples/<name>` paths persist in 4+ docs/tutorials and in
  `local-ci.md`/`cross-platform-setup.md` verification commands.
- **"Examples are documentation" bar not met** — multiple examples' README/header comments assert
  behaviour the code does not implement (Madgwick, sine, real peripheral opens, real inference).
- **Yocto layer not build-validated** — placeholder SRCREV/sha256/LIC across recipes; the layer's
  own bbappend flags itself "never run through bitbake" while README claims "build-validated".
- **Leak hygiene regression** — personal `C:\Users\…` paths in tracked Altium tooling + docs, and
  an untracked scratch file with home paths in the working tree.

---

## Suggested order of attack

1. **Stop-the-leak (fast, low-risk):** strip personal paths from `tools/altium/*` and
   `docs/local-ci.md`; delete/gitignore the `_*.txt` scratch files; sanitise the DT-patch headers.
2. **Canonicalisation sweep:** one pass replacing `deepx_dx`/`DEEPX_DX`/`dx-com`/`dx-compiler`/
   `deepx-dx-m1` → `DEEPX_DXM1`/`dxcom`, then add a `capabilities:` schema + a CI grep gate so it
   can't regress.
3. **Docs that lie about reality:** demote the AI-pipeline "operational" framing to its real
   status; fix the flat example paths; fix `bring-up-imx93.md` or mark it unsupported.
4. **Example correctness (High first):** UART0 double-open, DUMMY_DISPLAY in shared prj.conf,
   audio-loopback CMake, incompatible `alp_inference_config_t` fields, FFT block-size mismatch.
5. **Metadata correspondence:** add the driver↔manifest gate; author the 9 missing chip manifests;
   resolve the GD32 clock and RZ/V2N memory-region modelling.
6. **Yocto build-out (largest):** real SRCREV/sha256/LICENSE for the recipes; the V2M dtb patch;
   then re-run a real bake to validate.
