# Test Plan & Release Criteria

Living document.  **Every claimed feature lands here as soon as it's
coded.**  The row stays `untested` until verification evidence is
captured -- a green CI run on real silicon, an annotated screenshot,
a captured log, a HIL job ID, a recorded broker roundtrip, etc.

A release does **not** tag until every row gating it is `verified`.

---

## Verification key

- `⏳ untested` — code compiled + CI-linted; failure paths may be
  covered by unit tests but the **behavioural contract** has not
  yet been attempted against real hardware / a real broker / a
  real device.  This is the default for any newly-coded feature.
  Code in this state is documented as "claimed" rather than "GA".
- `🟡 partial` — failure paths covered (NULL / INVAL / ENOENT /
  close-NULL / parse errors) **and** the happy path has been
  exercised in some non-canonical environment (e.g. native_sim,
  a Docker broker, a developer's bench rig).  Real-target
  verification still pending.
- `✅ verified` — exercised against real silicon, a representative
  broker, or an integration target.  Notes column carries the
  evidence pointer (HIL run, log capture, dashboard screenshot, PR
  comment with reproduction).
- `❌ failing` — verification was **attempted** and the contract
  did **not** hold.  This is a bug -- the matching feature must
  be fixed before its gating release tags.  Treat this as a
  blocker, not a default state.
- `n/a` — feature is by design not directly testable (e.g. a doc-only
  stance, a removed surface).  Marked here for the audit trail.

## How to use this document

- **Adding a feature.**  Append a row in the right section before
  you push the commit.  Default status is `⏳ untested`.  Bump the
  status the moment evidence lands -- do not let it drift.  If
  verification is attempted and fails, mark `❌ failing` and link
  the failure capture; this is a blocker for the gating release.
- **Reviewing a release.**  Filter for "Gates" matching the version
  about to tag.  If any row is not `✅`, the release does not ship.
- **Linking evidence.**  Use inline links to CI runs, gist
  captures, or HIL artefacts.  Prefer artefacts that are persistent
  (GitHub Actions runs persist; ephemeral local shells do not).

## How this interacts with CI

- `pr-plain-cmake.yml` exercises the **failure paths** of every
  Yocto-side wrapper (NULL / INVAL / ENOENT / close-NULL) on a
  GitHub-hosted Ubuntu runner.  Passing this is necessary but not
  sufficient to flip a row from 🟡 to ✅.
- `nightly-aen-hil.yml` is the canonical AEN-Zephyr verifier --
  every AEN row whose Notes column references "nightly-aen-hil"
  flips to ✅ on a green run against a real E1M EVK.
- The `hil-yocto` self-hosted runner (parked in `docs/ci/HW-IN-LOOP.md`)
  is the canonical Yocto-side verifier; every 🟡 Yocto row gates on
  it before flipping to ✅.

---

## v0.1.0 — AEN-Zephyr foundation

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| AEN-Zephyr I²C real backend | `src/backends/i2c/zephyr_drv.c` | ⏳ untested | LSM6DSO WHOAMI = 0x6C read back on real E1M EVK via nightly HIL | `nightly-aen-hil` first green run | v0.1 |
| AEN-Zephyr SPI real backend | `src/backends/spi/zephyr_drv.c` | ⏳ untested | SPI flash JEDEC-ID (0x9F READID) returns expected bytes on real EVK | `nightly-aen-hil` | v0.1 |
| AEN-Zephyr UART real backend | `src/backends/uart/zephyr_drv.c` | ⏳ untested | Loopback 1 KiB at 115200 8N1, zero byte loss | `nightly-aen-hil` | v0.1 |
| AEN-Zephyr GPIO real backend | `src/backends/gpio/zephyr_drv.c` | ⏳ untested | Button press observed via IRQ; LED toggle visible on EVK | `nightly-aen-hil` | v0.1 |
| `<alp/soc_caps.h>` generation pipeline | `scripts/gen_soc_caps.py` | ⏳ untested | Generated header round-trips against tracked-in-repo reference snapshot | `pr-generated-files.yml` | v0.1 |
| ABI snapshot diff tool | `scripts/abi_snapshot.py` | ⏳ untested | Catches a deliberately injected breaking signature change | Manual diff test, recorded | v0.1 |

## v0.2.0 — peripheral expansion + capability validation

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| `<alp/pwm.h>` AEN-Zephyr | `src/backends/pwm/zephyr_drv.c` | ⏳ untested | LED dimmed to 25% / 50% / 75% observed via scope on EVK | HIL | v0.2 |
| `<alp/adc.h>` AEN-Zephyr | `src/backends/adc/zephyr_drv.c` | ⏳ untested | Reference voltage divider reads within ±2% of expected µV | HIL | v0.2 |
| `<alp/counter.h>` + QEnc AEN-Zephyr | `src/backends/counter/zephyr_drv.c` | ⏳ untested | Encoder ticks count up/down matching shaft direction | HIL | v0.2 |
| `<alp/i2s.h>` AEN-Zephyr | `src/backends/i2s/zephyr_drv.c` | ⏳ untested | 16 kHz 16-bit PDM-in captures known tone with FFT peak ±5 Hz | HIL | v0.2 |
| `<alp/can.h>` AEN-Zephyr | `src/backends/can/zephyr_drv.c` | ⏳ untested | CAN-FD frame sent + acknowledged against a second node | HIL | v0.2 |
| `<alp/rtc.h>` AEN-Zephyr | `src/backends/rtc/zephyr_drv.c` | ⏳ untested | Wall-clock advances 1 s ± kernel jitter over 60 s | HIL | v0.2 |
| `<alp/wdt.h>` AEN-Zephyr | `src/backends/wdt/zephyr_drv.c` | ⏳ untested | Watchdog reset observed when feed thread is starved | HIL | v0.2 |
| Real CC3501E Wi-Fi/BLE dispatch on AEN-Zephyr | `src/backends/wifi/cc3501e.c + src/backends/ble/cc3501e.c` | ⏳ untested | `alp_wifi_open()` selects the CC3501E provider and `alp_ble_open()` + scan complete on real AEN silicon | HIL via `aen-cc3501e-companion-tour` `PORTABLE_WIRELESS:` lines | v0.9 |
| EdgeAI vision reference app | `examples/aen/edgeai-vision-aen/` | ⏳ untested | ≥10 fps inference on real E1M EVK | HIL | v0.2 |

## v0.3.0 — IoT app, multi-proc, board.yaml

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| `<alp/inference.h>` Ethos-U on AEN | `src/backends/inference/tflm.cpp` | ⏳ untested | Vela-compiled MobileNetV2 outputs logits matching CPU reference within tolerance | HIL | v0.3 |
| `<alp/inference.h>` DEEPX DX-M1 (A55) | `src/yocto/inference_yocto.c` + `inference_deepx.cpp` | 🟡 code-complete, bench-unverified | Real `dxrt::InferenceEngine` body loads a `.dxnn` model + runs inference; outputs match a host-CPU reference | Body header-compiles against the **real** `dx_rt` headers (`dxrt::InferenceEngine`, replacing a fictional `dxnn_*` API); `tests/yocto/inference_dispatcher.c` covers NULL/INVAL; **real link needs the RZ/V Yocto sysroot, on-silicon run needs the DX-M1 PCIe card** | v0.8 |
| `<alp/inference.h>` DRP-AI (A55) | `src/yocto/inference_drpai.cpp` | 🟡 code-complete, bench-unverified | Real `MeraDrpRuntimeWrapper` body loads a `drpai_dir` model (tar staged to a tempdir) + runs inference | Body header-compiles against the **real** `MeraDrpRuntimeWrapper.h`; **cross-link needs the RZ/V Yocto SDK sysroot + EdgeCortix MERA libs, on-silicon run needs the V2N board** | v0.8 |
| `<alp/audio.h>` real impl | `src/backends/audio/zephyr_drv.c` | ⏳ untested | PDM mic captures audio playable through I²S DAC, no buffer underruns | HIL | v0.3 |
| `<alp/ble.h>` real impl | `src/backends/ble/zephyr_drv.c` + `src/backends/ble/cc3501e.c` | ⏳ untested | Advertise + connect + GATT read from a second BLE device; AEN provider opens and scans through CC3501E | HIL | v0.3 |
| `<alp/security.h>` real impl | `src/backends/security/zephyr_drv.c` | ⏳ untested | SHA-256 + AES-128-GCM round-trip against MbedTLS reference vectors | unit test or HIL | v0.3 |
| `<alp/mproc.h>` real impl | `src/backends/mproc/zephyr_drv.c` | ⏳ untested | M55-HP <-> M55-HE shared-memory mailbox echoes a payload | HIL | v0.3 |
| `board.yaml` loader (`scripts/alp_project.py`) | `scripts/alp_project.py` | 🟡 partial | Schema-level + capability-level checks unit-tested; cross-OS round-trips not exercised on hardware | `tests/scripts/test_project_loader.py`; **HIL exercise gates v0.3** | v0.3 |
| `validate_board_yaml.py` v0.3 capability cross-check | `scripts/validate_board_yaml.py` | 🟡 partial | Returns exit 3 on the deliberately-broken sample boards | `pr-metadata-validate.yml` | v0.3 |
| VS Code extension `Generate all` + inline diagnostics | [`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode) (separate repo since 2026-05-12) | ⏳ untested | Loaded into VS Code; commands run; problems panel surfaces validator errors | Manual capture (screencast/gist) | v0.3 |
| `<alp/hw_info.h>` EEPROM + BOARD_ID ADC read | `src/zephyr/hw_info_zephyr.c` | ⏳ untested | Real production-test fixture writes 128-byte manifest; firmware reads back identical | HIL + production-test bench | v0.3.x |
| `scripts/program_eeprom.py` packer | `scripts/program_eeprom.py` | 🟡 partial | Layout round-trips against the C reader's `static_assert` block | `tests/scripts/test_program_eeprom.py` | v0.3 |
| GD32G553 host-side bridge driver | `chips/gd32g553/` | ⏳ untested | `PING` + `GET_VERSION` round-trip on both SPI and I2C against the on-module bridge firmware on a real V2N SoM | HIL + `examples/v2n/v2n-gd32-bridge-ping` | v0.3.x |
| GD32 bridge firmware (`firmware/gd32-bridge/`) scaffold | `firmware/gd32-bridge/` | ⏳ untested | Build green with `BRIDGE_HAL_BACKEND=stub`; protocol round-trip via the host driver against the stub firmware on the GD32 silicon | HIL + smoke test | v0.3.x |
| RTL8211FDI Ethernet PHY driver | `chips/rtl8211fdi/` | ⏳ untested | PHYID1=0x001C read back; autoneg completes; `iperf3 > 900 Mb/s` against 1 Gb link partner on each of ET0 + ET1 | HIL via the HiL rig (plan maintained in the internal `alp-sdk-internal` repo) | v0.3.x |
| 5L35023B clock generator (partial) | `chips/clk_5l35023b/` | 🟡 partial | Byte 0x00 (General Control) ACK + I2C-strap match on V2N's BRD_I2C at 7-bit `0x68`; Byte 0x01 (Dash Code ID) read; soft power-down via Byte 0x24 `I2C_PDB`.  Register table cross-referenced into the driver header from the public Renesas Dec-2025 datasheet.  PLL / OUTDIV helpers stay out of scope -- V2N relies on the OTP defaults + the `Audio_CLKB_OE` GPIO. | NULL-arg + uninit-rejection + strap-decode ZTESTs green in `pr-twister`; **HIL exercise** for register-dump round-trip on real silicon | v0.3.x |
| Murata LBEE5HY2FY GPIO surface | `chips/murata_lbee5hy2fy/` | ⏳ untested | `WL_REG_ON` + `BT_REG_ON` toggles bring up the Wi-Fi / BT kernel devices on a Yocto image | HIL via `hil-yocto`; NULL-arg + uninit-rejection ZTESTs green in `pr-twister` | v0.3.x |
| DEEPX DX-M1 bring-up sequencer | `chips/deepx_dxm1/` | ⏳ untested | M1_RESET release + PCIe mux to DEEPX path -> `lspci` lists DX-M1 + `dxrt_init()` succeeds (V2N-M1 only) | HIL via the HiL rig; NULL-arg + uninit-rejection + polarity-validation ZTESTs green in `pr-twister` | v0.3.x |
| TPS628640 multi-instance buck (complete) | `chips/tps628640/` | 🟡 partial | Probe + VOUT1/2 R/W with `mv = byte * 5 + 400` encoding + STATUS read-and-clear (thermal-warning / HICCUP / UVLO bits) + CONTROL typed helpers (software-enable, FPWM mode, ramp speed, factory-reset) + raw register R/W.  Register table cross-referenced into the header against TI SLVSEI1C (October 2020 rev).  CONTROL shadow seeded from the datasheet default (0x6E) so write-only-register updates don't clobber unrelated bits. | NULL-arg + uninit-rejection + range-check + ramp-speed-invalid ZTESTs green in `pr-twister`; **HIL VOUT readback + STATUS-bit assertion** on real silicon gates flip to ✅ | v0.3.x |
| GD32 SWD bit-bang controller (partial) | `chips/gd32_swd/` | 🟡 partial | Packet layer + DPIDR read + Cortex-M33 halt + FMC erase/write/verify wired against the Arm ADIv5 + GD32G553 user-manual register layout.  Pad assignments resolved (SWDIO/SWCLK/NRST → Renesas P70/P71/P74; see `metadata/chips/gd32_swd.yaml`). | First-silicon SWD round-trip on real V2N; matches expected IDCODE `0x6BA02477` | v0.3.x |
| GD32 application-bootloader OTA opcodes | `firmware/gd32-bridge/src/bootloader/` | ⏳ untested | All seven OTA opcodes (`CMD_OTA_BEGIN..CMD_OTA_ABORT`) routed through `bl_dispatch_ota`; bodies return `STATUS_NOSUPPORT` until FMC HAL lands; `CMD_OTA_GET_STATE` is read-only and replies concretely. | Wire round-trip against the host driver's matching opcode helpers (lands when those land); FMC HAL exercise on real silicon | v0.3.x → v0.4 (real bodies) |
| Per-SoM hw-revision change-log enforcement | `scripts/validate_metadata.py` + `metadata/e1m_modules/<family>/hw-revisions.yaml` | 🟡 partial | Validator covers structural schema; `examples/v2n/v2n-board-id-readout` reads the EEPROM manifest and asserts it matches the firmware build.  Runtime BOARD_ID ADC cross-check still stubbed pending the per-family generated header. | `pr-metadata-validate.yml` green; **HIL BOARD_ID ADC** exercise gates flip to ✅ | v0.3.x |

## v0.4.0 — Yocto first-class, secure boot, OTA

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| AEN-Zephyr UART RX ring buffer (LwRB) | `src/backends/uart/zephyr_drv.c` (RX ringbuf section) | 🟡 partial | IRQ-driven attach delivers a known 1 KiB burst into the caller's ring with zero byte loss over 10 consecutive bursts | `tests/zephyr/peripheral/` (NULL/INVAL failure paths) + `examples/peripheral-io/uart-rx-ringbuf` (attach -> pop -> detach happy path on native_sim); **real-IRQ attach via `nightly-aen-hil`** | v0.4 |
| AEN-Zephyr `<alp/mproc.h>` IPC envelope framing (placeholder) | `src/common/proto/alp_mproc_frame.{h,c}` + `src/backends/mproc/zephyr_drv.c` framing branches | 🟡 partial | A real M55-HP <-> M55-HE roundtrip echoes the wrapped envelope unchanged through `alp_mbox_send` -> peer -> `alp_mbox_msg_cb_t` | `tests/unit/mproc_registry/` (9 framing ZTESTs cover encode/decode + the `alp_sdk.mproc.nanopb_framing` twister scenario compiles the framing branch in `alp_mbox_send`); **peer-firmware roundtrip via `nightly-aen-hil`** | v0.4 (placeholder, shipping); real nanopb wire interim/deferred, no committed version |
| Yocto I²C wrapper (i2c-dev) | `src/yocto/peripheral_i2c.c` | 🟡 partial | LSM6DSO WHOAMI = 0x6C reads back over real `/dev/i2c-N` on a Yocto target | `tests/yocto/peripheral_i2c.c` (failure paths only); **HIL via `hil-yocto`** | v0.4 |
| Yocto SPI wrapper (spidev) | `src/yocto/peripheral_spi.c` | 🟡 partial | SPI flash JEDEC-ID via `/dev/spidev<bus>.<cs>` returns expected bytes | `tests/yocto/peripheral_spi.c`; **HIL via `hil-yocto`** | v0.4 |
| Yocto UART wrapper (termios) | `src/yocto/peripheral_uart.c` | 🟡 partial | TX/RX loopback at 115200 8N1, zero byte loss over 1 KiB | `tests/yocto/peripheral_uart.c`; **HIL via `hil-yocto`** | v0.4 |
| Yocto GPIO wrapper (chardev v2) | `src/yocto/peripheral_gpio.c` | 🟡 partial | Output toggle observable on scope; input level reads back through `/dev/gpiochipN` | `tests/yocto/peripheral_gpio.c`; **HIL via `hil-yocto`** | v0.4 |
| Yocto GPIO IRQ dispatcher (pthread `poll()`) | `src/yocto/peripheral_gpio.c` (IRQ section) | ⏳ untested | Real edge event on real `/dev/gpiochipN` fires the registered callback within 5 ms | **HIL via `hil-yocto`** | v0.4 |
| Yocto MQTT (libmosquitto) | `src/backends/mqtt/yocto_drv.c` (+ `src/mqtt_dispatch.c`, #33 registry migration) | 🟡 partial | Connect + publish + subscribe + wildcard match roundtrip against a Mosquitto broker (local or cloud) | `tests/yocto/iot_mqtt.c` (URI parse + NULL-arg only); **broker roundtrip via `hil-yocto`** | v0.4 |
| Yocto MQTT TLS (`mqtts://`) | `src/backends/mqtt/yocto_drv.c` (`apply_tls`) | 🟡 partial | Connect + handshake against a TLS broker with a pinned CA bundle; reject on bad cert | `tests/yocto/iot_mqtt.c` (default-TLS open / pinned-CA open / missing-CA refusal / insecure-flag accepted / default-8883 port); **broker handshake via `hil-yocto`** | v0.4 |
| Yocto audio backend (ALSA) | `src/backends/audio/yocto_drv.c` (+ `src/audio_dispatch.c`, #33 registry migration) | 🟡 partial | 1 kHz tone captured + played back via ALSA on a real Yocto target; FFT peak within ±5 Hz; volume scale audibly correct | `tests/yocto/audio_alsa.c` (NULL/INVAL + unreachable device failure paths); **real capture/playback via `hil-yocto`** | v0.4 |
| Yocto `<alp/security.h>` (OpenSSL) | `src/backends/security/yocto_drv.c` (+ `src/security_dispatch.c`, #33 registry migration) | 🟡 partial | KATs + AEAD round-trip on a representative Yocto image; same wins on a developer-bench Linux host (the ALP_OS=yocto build IS the runtime target -- crypto correctness is host-portable) | `tests/yocto/security_openssl.c` (16 tests: SHA-256 NIST `"abc"` KAT, SHA-384/512 length checks, AES-128-GCM + ChaCha20-Poly1305 roundtrips, tag-mismatch path, key-length / NULL-key / unsupported-alg refusals, TRNG fill + null-arg).  Flips to ✅ once `meta-alp-sdk` builds a real Yocto image carrying this code on a SoM. | v0.4 |
| Yocto `<alp/rtc.h>` (`/dev/rtcN`) | `src/backends/rtc/yocto_drv.c` | 🟡 code-complete, link-unverified | Wall-clock read/set via `RTC_RD_TIME`/`RTC_SET_TIME` against a real `/dev/rtc0`; `alp_rtc_capabilities()` resolves (registry/dispatcher migration, issue #33) | Backend + dispatcher compile clean (`gcc -std=gnu11`); nm-audited single-symbol ownership; **full Yocto link + on-target ioctl run need the sysroot (`hil-yocto`)** | v0.8 |
| Yocto `<alp/wdt.h>` (`/dev/watchdogN`) | `src/backends/wdt/yocto_drv.c` | 🟡 code-complete, link-unverified | Timeout set + keepalive + magic-close via `WDIOC_*` against `/dev/watchdog0` (issue #33) | same compile + nm verification as the RTC row; **on-target run gates `hil-yocto`** | v0.8 |
| `meta-alp-sdk` BSP for V2N / V2N-M1 / i.MX 93 | `meta-alp-sdk/` | 🟡 partial | `bitbake alp-image-edge` succeeds for each `e1m-<sku>-a55` MACHINE; produced image boots on the matching SoM | V2N leg: full `core-image-minimal` bake clean on BSP v6.30 (8313 tasks, 2026-05-26, kernel 6.1.141-cip43 + carrier dtb + FIP) and the produced kernel/dtb run the live V2N bench board from eMMC.  `alp-image-edge` (dev) + `alp-image-prod` (hardened) now share `alp-image-common.inc` and bake under the `alp` distro; the V2N image boots the bench board.  V2N-M1 + i.MX 93 machines still pending | v0.7 |
| Authoritative Zephyr board files | external (`alplabai/alp-zephyr-modules`) | ⏳ untested | `alp_e1m_evk_aen` board boots and binds the same DT aliases the SDK overlays expect | HIL | v0.4 |
| MCUboot secure-boot on AEN-Zephyr | `zephyr/sysbuild/aen/sysbuild.conf` + `keys/generate_dev_key.sh` + `docs/secure-boot.md` | ⏳ untested | Signed image boots; tampered image triggers rollback to previous slot; mid-swap power loss recovers atomically | HIL + tamper test (gates on `alp_e1m_evk_aen` board file in `alplabai/alp-zephyr-modules`) | v0.4 |
| Secure OTA on AEN-Zephyr (MCUboot + Mender) | `docs/ota.md` (Zephyr-client decision pending) | ⏳ untested | Signed update delivered, swap-using-scratch completes, next boot validates | HIL + OTA bench (gates on Zephyr-side Mender client choice: `mender-mcu-client` vs Hawkbit, plus `alp_e1m_evk_aen` board file) | v0.4 |
| Secure OTA on V2N-Yocto (`meta-mender`) | `meta-alp-sdk/conf/distro/include/mender.inc` + machine .conf opt-in lines + `docs/ota.md` | ⏳ untested | A/B partition swap survives an interrupted-update simulation; rollback on failed commit health-check | HIL via `hil-yocto` (the A55/Mender path; distinct from the GD32 supervisor A/B OTA verified in the v0.6 section below) | v0.7 |
| Secure OTA on i.MX 93-Yocto (`meta-mender`) | same scaffolding as V2N row above | ⏳ untested | A/B partition swap survives an interrupted-update simulation | HIL via `hil-yocto` | v0.4 |
| OPTIGA Trust M-rooted device identity | TBD | ⏳ untested | TLS handshake succeeds using OPTIGA-stored ECC key; tampered key rejects | HIL | v0.4 |
| DEEPX DX-M1 on-silicon link + run | `src/yocto/inference_deepx.cpp` | ⏳ untested | The now-real `dxrt::InferenceEngine` body links against `dx_rt` on the Yocto sysroot; a sample `.dxnn` model run completes; outputs match host-CPU reference | HIL (DX-M1 PCIe card) | v0.8 |
| Ethos-U65 real attach on i.MX 93 | `src/backends/inference/ethos_u_n93.cpp` | ⏳ untested | Vela-compiled model run on i.MX 93 NPU; outputs match Ethos-U55 reference | HIL | v0.4 |
| DRP-AI3 real attach on V2N | `src/yocto/inference_drpai.cpp` | ⏳ untested | The now-real `MeraDrpRuntimeWrapper` body cross-links against the RZ/V Yocto SDK + EdgeCortix MERA libs; DRP-AI-translator output runs on V2N silicon | HIL | v0.8 |

## v0.4 prep — landed on `main` (2026-05-11)

These rows are duplicated from v0.4 above so the "what's on `main`
right now" status is one scroll away from the v0.4 gate list:

| Feature | Status | Evidence |
|---|---|---|
| Yocto core-4 peripherals (I²C / SPI / UART / GPIO) | 🟡 partial — failure-path ctest green in `pr-plain-cmake`; real-hardware untested | `pr-plain-cmake.yml` run on commit a0dadb8+ |
| Yocto GPIO IRQ dispatcher | ⏳ untested | (No real-edge test exists yet) |
| Yocto MQTT via libmosquitto | 🟡 partial — URI parser + NULL-arg paths covered by `tests/yocto/iot_mqtt.c`; broker roundtrip untested | `pr-plain-cmake.yml` run on commit 1965c4f+ |
| Coverity workflow wiring | ✅ verified | <https://github.com/alplabai/alp-sdk/actions/runs/25673163492> + first scan at <https://scan.coverity.com/projects/alplabai-alp-sdk> |
| `west.yml` `extras-lwrb-nanopb` group with lwrb + nanopb pins | 🟡 partial — pins resolve via `west update --group-filter +extras-lwrb-nanopb` on a fresh workspace; first LwRB consumer (UART RX ringbuf) landed against the in-tree stub impl; first nanopb consumer landed against a placeholder framing impl (not the generator-emitted codec yet).  Interim/deferred as of v0.9 -- no committed version flips the group on | (Stub-side coverage in `alp_sdk.peripheral.uart_rx_ringbuf` + `alp_sdk.mproc.nanopb_framing` twister scenarios; upstream-on-workspace builds untested) |
| AEN-Zephyr UART RX ring buffer (LwRB) | 🟡 partial — failure-path ZTESTs green in `pr-twister` (both the default and `prj_uart_ringbuf.conf` scenarios); real-IRQ attach untested | `pr-twister.yml` `alp_sdk.peripheral.uart_rx_ringbuf` scenario |
| AEN-Zephyr mproc envelope framing (placeholder) | 🟡 partial — 9 framing helper ZTESTs + the `alp_sdk.mproc.nanopb_framing` scenario compile the framing branches in `alp_mbox_send` / `mbox_rx_cb`; peer-firmware roundtrip untested | `pr-twister.yml` `alp_sdk.mproc.nanopb_framing` scenario |

## v0.5.0 — AEN accelerator surfaces (vendor-HAL-gated)

The 2026-05-12 AEN feature audit surfaced three on-die AEN-family
accelerator blocks with no portable / vendor-ext surface.  Each
header + stub landed in v0.5 ahead of the Alif HAL pack: every
function returns `ALP_ERR_NOSUPPORT` until the matching vendor pack
ships, so these rows are **gated on the vendor HAL pack and stay
`⏳ untested` until first silicon** (no SoM in scope ships the HAL
pack today).  Design captured in
[`docs/aen-accelerator-backends-design.md`](aen-accelerator-backends-design.md).

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| GPU2D (D/AVE 2D) portable surface | `src/backends/gpu2d/sw_fallback.c` (real CPU path) + `src/backends/gpu2d/alif_dave2d.c` (D/AVE 2D real backend, bench-unverified, issue #24) | ✅ sw_fallback unit-tested / ⏳ D/AVE 2D untested | sw_fallback `fill_rect` / `blit` / `blend` produce exact pixels asserted in `tests/unit/gpu2d_registry` on native_sim; the D/AVE 2D backend produces the same pixels on a real E1M-AEN EVK | `tests/unit/gpu2d_registry` pixel ZTESTs green in `pr-twister`; **HAL-backed pixel assertion via `nightly-aen-hil`** — gated on the Alif D/AVE 2D vendor pack | sw_fallback v0.x (tested), D/AVE 2D bench-gated |
| ISP (VeriSilicon ISP Pico (vsi,isp-pico)) vendor-ext surface | `src/backends/ext/alif/camera.c` + `<alp/ext/alif/camera.h>`. **The ISP Pico backend registers `priority = 100` on `silicon_ref = "alif:ensemble:e8"` only** (`src/backends/camera/alif_isp_pico.c`) — E4/E6 carry the ISP in silicon but get no backend registration today, and every `ext/alif/camera.c` entry point returns `ALP_ERR_NOSUPPORT` | ⏳ untested | 3A window / per-channel gain LUT / LSC MESH LUT take effect on real ISP Pico silicon (the ISP is present on E4/E6/E8 — **not E7** — though only E8 is wired up); non-Alif handle returns `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` | NULL/INVAL + vendor-handle-gate ZTESTs green in `pr-twister`; **HAL-backed ISP statistics readback** on ISP Pico silicon — gated on the Alif ISP Pico HAL pack | v0.5 (header+stub), real bodies gate v0.x |
| Inline-AES (OSPI SecAES) vendor-ext surface | `src/backends/ext/alif/storage.c` + `<alp/ext/alif/storage.h>` | ⏳ untested | SecAES key-provision binds a slot and encrypted XIP executes from OctalSPI on real ISP-Pico-tier silicon (E4/E6/E8); status read-back reports `ENGAGED` before XIP is trusted | NULL/INVAL + vendor-handle-gate ZTESTs green in `pr-twister`; **encrypted-XIP boot + status readback** on silicon — gated on the Alif SecAES HAL pack | v0.5 (header+stub), real bodies gate v0.x |

## v0.6.0 — V2N GD32-bridge silicon campaign (verified on the bench)

The first rows in this ledger verified against real silicon: the
E1M-X V2N bench board (RZ/V2N CM33 host ↔ GD32G553 supervisor,
firmware v0.2.9, wire protocol v0.7).  Evidence pointers are the
merge commits on `main` (each names its bench results) + the
matching CHANGELOG `[v0.6.0]` entries.

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| GD32 bridge link (SPI 25 MHz + I2C), full command set | `firmware/gd32-bridge/` + `chips/gd32g553/` | ✅ verified | Functional suite asserts real values (ADC/DAC/PWM/math/TRNG fault-recover contract) on silicon | 26/26 on fw v0.2.4 (merge `ed1daf2`); re-smoked through v0.2.9 | v0.6 |
| Bridge HIL soak (20 rows, link-stability) | `examples/v2n/v2n-gd32-bridge-hil-soak/` | ✅ verified | Every row passes a sustained soak with zero IO errors; counter-row stale-reply discriminator clean | 253/253 × 20 rows on v0.2.8 + v0.2.9 (2026-06-06) | v0.6 |
| Protocol v0.7 STATUS_SEQ stale-reply kill | `firmware/gd32-bridge/src/transport_spi.c` + host driver | ✅ verified | Negotiated stamp advances per fresh decode; stale replies detected + safely re-sent on silicon | CHANGELOG v0.2.9 entry; `seq_forensics` telemetry in the soak | v0.6 |
| GD32 supervisor A/B OTA (Path A, opcodes 0xF0..) | `firmware/gd32-bridge/src/ota.c` + bootloader | ✅ verified | Full image stream → CRC verify → slot swap → boot-back, e2e on silicon | OTA bench e2e (7 silicon bugs found + fixed; memory ledger `project-gd32-ota-path-a-validated`) | v0.6 |
| Tier-B analog loopback (DAC→ADC, PWM-capture) | `examples/v2n/v2n-gd32-bridge-loopback/` | ✅ verified | DAC raws within 1 LSB of commanded through the carrier jumper; capture period/pulse exact | 5/6 rows (qenc blocked by carrier ENC1_Y float — issue #85) on v0.2.7+ | v0.6 |
| CM33↔GD32 SCI7 SPI bidirectional link | `zephyr/drivers/spi/` RSCI path | ✅ verified | Sustained request/reply traffic, both transports, zero CRC errors in soak | merges `9f3e600`, `7845ad7`, `b5c941c` | v0.6 |
| SCI7 DMA fast path | `zephyr/drivers/spi/spi_renesas_rz_sci_b.c` DMAC-B path | ❌ failing | DMA-driven transactions sustain the soak | Survives full init incl. v0.7 negotiation, then TX requests stop post-settle (issue #84; vendor ticket drafted); gate stays default-off | v0.7 |

## v0.9.0 candidate — portable-surface consistency batch (2026-07)

The 2026-07 CX batch (PRs #319..#335): `<alp/version.h>`, the SE-backed
SoC-identity / power-profile / core-boot surfaces, I²C/SPI target
(slave) mode + `alp_init`/`alp_deinit`, DAC capabilities + the analog/WDT
API-consistency renames, the `alp` CLI front door, and the portable-API
conformance suite.  native_sim coverage lands with the code; every
silicon-facing row is bench-gated as usual.

| Feature | Module / file | Status | What "verified" means | Evidence | Gates |
|---|---|---|---|---|---|
| `<alp/version.h>` macros + `alp_version_string()` | `include/alp/version.h` + `src/common/version.c` | 🟡 partial | Compile-time macros and the runtime string agree with `metadata/sdk_version.yaml` on every build | `scripts/check_version_doc_sync.py` (CI) + `tests/smoke.c`; nothing silicon-facing — flips ✅ with two consecutive green gate runs | v0.9 |
| I²C target (slave) mode (`alp_i2c_target_*`) | `src/backends/i2c/zephyr_drv.c` (target section) + `src/i2c_dispatch.c` | 🟡 partial | An external I²C controller reads/writes the register file `examples/peripheral-io/i2c-slave` exposes at 0x42 on real silicon | `tests/zephyr/peripheral/src/i2c_target.c` ztests (reserved-address / callback validation + emul open/close roundtrip) + example degrade-path on native_sim (registration accepted, callbacks external-controller-gated); **two-board exercise via HIL** | v0.9 |
| I²C register-file target helper (`alp_i2c_regfile_*`) | `include/alp/i2c_regfile.h` + `src/i2c_regfile.c` | 🟡 partial | An external I²C controller pointer-latches, reads/writes, and EEPROM-wraps the register file on real silicon, and stats count payload bytes only | `tests/zephyr/i2c_regfile/src/main.c` ztests (pointer-latch + repeated-START + modulo-len wraparound + payload-only traffic stats + NULL-handle/closed-handle INVAL on the native_sim emul) + `examples/peripheral-io/i2c-device-hub` degrade path on native_sim; shares the `alp_i2c_target_*` **two-board exercise via HIL** | v0.9 |
| Console companion binder (`alp_console_companion_set`) | `include/alp/ext/cc3501e/console.h` + `src/zephyr/console/` (companion binder) | 🟡 partial | Binding a companion handle lets `alp companion ver`/`ping` reach a live CC3501E/GD32; with none bound the verbs report "companion not registered" instead of faulting | No dedicated ztest yet — native_sim coverage is the `examples/peripheral-io/alp-console` build (`testcase.yaml`), where the unbound path degrades to "companion not registered"; **live-companion exercise via the `alp companion` CLI verb on HIL** | v0.9 |
| SPI target (slave) mode (`alp_spi_target_*`) | `src/backends/spi/zephyr_drv.c` (`SPI_OP_MODE_SLAVE`) + `src/spi_dispatch.c` | 🟡 partial | `examples/peripheral-io/spi-slave`'s PING/GET_VERSION protocol answers a real external master frame-for-frame | `tests/zephyr/peripheral/src/spi_target.c` ztests (open-time config validation, len==0/NULL-arg contract, bounded-wait NOSUPPORT degrade on sync-only builds, close idempotence) on native_sim; **two-board exercise (incl. real bounded-wait timeout under CONFIG_SPI_ASYNC) via HIL** | v0.9 |
| `alp_init` / `alp_deinit` lifecycle | `src/common/` init path | 🟡 partial | Idempotency contract holds (double-init OK, deinit-then-reopen OK) on native_sim + real target | Every `examples/peripheral-io/*` example calls it first; conformance suite runs on native_sim | v0.9 |
| SoC identity over SE (`alp_soc_info_read` / `alp_soc_secure_fw_ping`) | `src/backends/soc_info/alif_se.c` (+ `sw_fallback.c`) | ⏳ untested | Part number / die rev / lifecycle / serial read back on a real E8 match SETOOLS' view; ping bounded round-trip | sw_fallback NOSUPPORT-with-soc_ref path covered on native_sim; **SE round-trip via `nightly-aen-hil`** | v0.9 |
| Power profiles over SE (`alp_power_profile_get/_set`) | `src/backends/power/` profile backend (`power_profile` class) | ⏳ untested | RUN/STANDBY profile read matches the SE's view; a set() round-trips exactly (no rounding) without brown-out on a real E8 | NULL/INVAL paths on native_sim; **SE exercise via `nightly-aen-hil`** — treat set() like a firmware update on the bench | v0.9 |
| Peer-core boot (`alp_mproc_boot_core`) | `src/backends/mproc/` boot backend (`mproc_boot` class) | ⏳ untested | M55-HE released at a valid entry and its firmware handshakes back (the AEN dual-core examples) | `ALP_CORE_SELF` rejection on native_sim; **dual-core release via `nightly-aen-hil`** | v0.9 |
| `alp_dac_capabilities()` | `src/dac_dispatch.c` + backends | 🟡 partial | Caps reported on an opened handle match the silicon (resolution, channel count) on a real target | Live capabilities row in the conformance suite on native_sim | v0.9 |
| `alp_wdt_open(const alp_wdt_config_t *)` single-arg + ADC `_mv` read renames | `include/alp/wdt.h` / `include/alp/adc.h` + all in-tree callers | 🟡 partial | Pre-1.0 signature migration: no stale two-arg / unsuffixed callers anywhere in tree; behaviour unchanged | Conformance suite adopted the new signatures (`eec8868d`); grep-clean tree; ABI snapshot regenerated | v0.9 |

## CI-only / tooling rows (no HIL gate)

These never need HIL.  They tag `✅` once the matching workflow has
been green on `main` for at least two consecutive PRs.

| Feature | Workflow | Status |
|---|---|---|
| `pr-plain-cmake.yml` builds + tests | `pr-plain-cmake.yml` | ✅ verified |
| `pr-twister.yml` ztest matrix | `pr-twister.yml` | ✅ verified |
| `pr-static-analysis.yml` clang-format diff | `pr-static-analysis.yml` | ✅ verified |
| `pr-generated-files.yml` (`soc_caps.h` + ABI snapshot drift) | `pr-generated-files.yml` | ✅ verified |
| `pr-metadata-validate.yml` (`board.yaml` schema + loader smoke) | `pr-metadata-validate.yml` | ✅ verified |
| `pr-doxygen.yml` zero-warnings | `pr-doxygen.yml` | ✅ verified |
| VS Code extension build (split repo) | [`alplabai/alp-sdk-vscode` &mdash; ci workflow](https://github.com/alplabai/alp-sdk-vscode/actions/workflows/ci.yml) | ✅ verified |
| `coverity.yml` weekly scan | `coverity.yml` | ✅ verified |
| Portable-API conformance suite (13 classes × 8 cases, `alp_sdk.conformance.portable_api`) | `pr-twister.yml` (`tests/zephyr/conformance/`) | ✅ verified |
| `alp` CLI verbs (`init` / `new-som` / `build` / `run` / `flash` / `emit` / `validate` / `doctor` / `monitor`) | click-runner pytest (`tests/scripts/test_alp_cli*.py`) via `pr-metadata-validate.yml` + `cross-platform-zephyr.yml` | ✅ verified |

---

## Anti-checklist (things NOT verified by this doc)

- **Performance numbers** -- this doc is correctness only.  Throughput,
  jitter, and RAM/flash footprints live in `tests/bench/` and produce their
  own captures.
- **Security audit** -- "no obvious vulnerabilities" is not on the
  list.  Coverity + the v1.0 prep cycle's external audit handle that.
- **Studio-side integration** -- whether alp-studio generates correct
  calls into the SDK is verified by the studio repo's own CI; we
  trust its reports.
- **End-customer apps** -- a green row here proves the SDK contract
  holds, not that every customer app built on top works.

## See also

- [`VERSIONS.md`](../VERSIONS.md) — versioned roadmap; this doc is
  the verification ledger that gates each version's tag.
- [`docs/ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md) — HIL runner contract.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — every new feature must
  append a row here in the same PR.
