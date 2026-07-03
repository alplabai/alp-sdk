# OS Support Matrix

Status keys:
- **GA** — implemented **and verified end-to-end** on real silicon, in CI.
  Verification evidence is captured in [`test-plan.md`](test-plan.md).
- **code complete (untested)** — implementation merged + CI-linted;
  no behavioural proof against real hardware yet.  See
  [`test-plan.md`](test-plan.md) for what "verified" means per row.
- **stub** — header surface compiles; functions return `ALP_ERR_NOSUPPORT`.
- **planned** — declared roadmap, no code yet.
- **n/a** — combination not targeted.

> **Calibration note (2026-06-24; updated 2026-06-24).** v0.8.0 ships
> silicon-verified silicon: V2N GD32-bridge campaign (since v0.6),
> AEN801 (E8) peripheral matrix (15/17 apps PASS on real silicon, 2
> PARTIAL hardware-gated), and cc3501e bridge (hardware SS0, real
> BLE/Wi-Fi scan, GPIO proxy, production warm-program flow) — all on
> real silicon.  The **GA** rows marking code-complete AEN-Zephyr work
> reference pre-v0.8 maturity; see the v0.8.0 CHANGELOG for what is now
> silicon-verified.

Each column targets a **`<SoM>: <core_id> <runtime>`** triple — the
SDK now builds each on-die programmable core independently.  Within
a family every SKU shares the same E1M routing and the same vendor
HAL, so a single backend covers the whole family.  See
[`README.md` § Supported hardware](../README.md#supported-hardware)
for the SKU breakdown:

- **E1M-AEN family** — `E1M-AEN301` … `E1M-AEN801` (Alif Ensemble
  E3–E8).  E3 / E4 are RTOS-only (no A-class); E5..E8 carry an
  A32 cluster alongside the M55 pair.
- **E1M-X V2N family** — `E1M-V2N101`, `E1M-V2N102` (Renesas RZ/V2N):
  A55 cluster + M33-SM.
- **E1M-X V2N-M1 family** — `E1M-V2M101`, `E1M-V2M102` (RZ/V2N +
  DEEPX DX-M1): same topology as V2N.
- **E1M-N93 family** — iMX93: A55 cluster + M33.

Because the per-core matrix is 11 columns wide, the per-version
tables below are split into a **Cortex-A (Yocto)** table and a
**Cortex-M (Zephyr)** table.  Read both for a given SoM to get the
full per-(library × core × runtime) picture.

## v0.1.0

### Cortex-A (Yocto)

| Library     | AEN E5..E8: a32_cluster Yocto | V2N: a55_cluster Yocto | V2N-M1: a55_cluster Yocto | iMX93: a55_cluster Yocto |
|-------------|-------------------------------|------------------------|---------------------------|--------------------------|
| Peripherals (I2C/SPI/GPIO/UART) | stub | stub | stub | planned |
| Display     | stub                          | stub                   | stub                      | planned |
| Camera      | stub                          | stub (planned v0.2 MIPI CSI-2) | stub (planned v0.2) | planned |
| GUI/LVGL    | planned                       | planned                | planned                   | planned |
| IoT         | stub                          | stub                   | stub                      | planned |

### Cortex-M (Zephyr)

| Library     | AEN E3/E4: m55_hp Zephyr | AEN E3/E4: m55_he Zephyr | AEN E5..E8: m55_hp Zephyr | AEN E5..E8: m55_he Zephyr | V2N: m33_sm Zephyr | V2N-M1: m33_sm Zephyr | iMX93: m33 Zephyr |
|-------------|--------------------------|--------------------------|---------------------------|---------------------------|--------------------|----------------------|--------------------|
| Peripherals (I2C/SPI/GPIO/UART) | **GA** | **GA** | **GA** | **GA** | stub | stub | stub |
| Display     | **GA** (SSD1306)         | **GA** (SSD1306)         | **GA** (SSD1306)          | **GA** (SSD1306)          | stub               | stub                 | stub               |
| Camera      | stub                     | stub                     | stub                      | stub                      | stub               | stub                 | stub               |
| GUI/LVGL    | **GA** (re-export)       | **GA** (re-export)       | **GA** (re-export)        | **GA** (re-export)        | stub               | stub                 | stub               |
| IoT         | stub (real Wi-Fi+MQTT in v0.2) | stub (real Wi-Fi+MQTT in v0.2) | stub | stub | stub | stub | stub |

> **Note:** CMSIS-DSP is consumed directly via `arm_math.h` from app code; the SDK does not re-export it.

> **Note on the Display "GA (SSD1306)" cells:** that GA claim covers
> the **SSD1306 chip driver** (`chips/ssd1306/`, `ssd1306_*`) and
> the LVGL re-export (`<alp/gui.h>`) riding Zephyr's own display
> binding — **not** the portable `<alp/display.h>` class, whose
> backend was the NOT_IMPLEMENTED stub until issue #23 landed the
> Zephyr driver-class wrapper (see the v0.5+ surfaces table).

## v0.2.0 — landed (peripheral expansion + capability validation)

The v0.2 SDK doubles peripheral coverage from 4 to 12 wrapped
classes plus the diagnostic / validation infrastructure that
underpins them.  Surface-only deliverables ship the public header
+ a stub backend; full implementations land per the per-class
plan in `VERSIONS.md`.

### Cortex-A (Yocto)

| Library                   | AEN E5..E8: a32_cluster Yocto | V2N: a55_cluster Yocto | V2N-M1: a55_cluster Yocto | iMX93: a55_cluster Yocto |
|---------------------------|-------------------------------|------------------------|---------------------------|--------------------------|
| **PWM** (`<alp/pwm.h>`)   | code complete¹                | code complete¹         | code complete¹            | code complete¹            |
| **ADC** (`<alp/adc.h>`)   | code complete¹                | code complete¹         | code complete¹            | code complete¹            |
| **Counter / QEnc** (`<alp/counter.h>`) | code complete¹  | code complete¹         | code complete¹            | code complete¹            |
| **I²S / SAI** (`<alp/i2s.h>`) | code complete¹ (ALSA)     | code complete¹ (ALSA)  | code complete¹ (ALSA)     | code complete¹ (ALSA)     |
| **CAN / CAN-FD** (`<alp/can.h>`) | code complete¹         | code complete¹         | code complete¹            | code complete¹            |
| **RTC** (`<alp/rtc.h>`)   | code complete¹                | code complete¹         | code complete¹            | code complete¹            |
| **Watchdog** (`<alp/wdt.h>`) | code complete¹             | code complete¹         | code complete¹            | code complete¹            |
| **Audio** (`<alp/audio.h>`) | code complete¹              | code complete¹         | code complete¹            | code complete¹            |
| **Camera** (`<alp/camera.h>`) | planned                   | **GA** (MIPI CSI-2)    | **GA**                    | planned |
| **IoT** (`<alp/iot.h>`)   | **GA**                        | **GA**                 | **GA**                    | planned |

### Cortex-M (Zephyr)

| Library                   | AEN E3/E4: m55_hp Zephyr | AEN E3/E4: m55_he Zephyr | AEN E5..E8: m55_hp Zephyr | AEN E5..E8: m55_he Zephyr | V2N: m33_sm Zephyr | V2N-M1: m33_sm Zephyr | iMX93: m33 Zephyr |
|---------------------------|--------------------------|--------------------------|---------------------------|---------------------------|--------------------|----------------------|--------------------|
| **PWM** (`<alp/pwm.h>`)   | **GA** (Zephyr `pwm_*`)  | **GA** (Zephyr `pwm_*`)  | **GA** (Zephyr `pwm_*`)   | **GA** (Zephyr `pwm_*`)   | stub               | stub                 | stub               |
| **ADC** (`<alp/adc.h>`)   | **GA** (Zephyr `adc_*`)  | **GA** (Zephyr `adc_*`)  | **GA** (Zephyr `adc_*`)   | **GA** (Zephyr `adc_*`)   | stub               | stub                 | stub               |
| **Counter / QEnc** (`<alp/counter.h>`) | **GA** (Zephyr `counter_*` + `sensor_*`) | **GA** (Zephyr `counter_*` + `sensor_*`) | **GA** (Zephyr `counter_*` + `sensor_*`) | **GA** (Zephyr `counter_*` + `sensor_*`) | stub | stub | stub |
| **I²S / SAI** (`<alp/i2s.h>`) | **GA** (Zephyr `i2s_*`) | **GA** (Zephyr `i2s_*`) | **GA** (Zephyr `i2s_*`)   | **GA** (Zephyr `i2s_*`)   | stub               | stub                 | stub               |
| **CAN / CAN-FD** (`<alp/can.h>`) | **GA** (Zephyr `can_*`) | **GA** (Zephyr `can_*`) | **GA** (Zephyr `can_*`) | **GA** (Zephyr `can_*`) | stub               | stub                 | stub               |
| **RTC** (`<alp/rtc.h>`)   | **GA** (Zephyr `rtc_*`)  | **GA** (Zephyr `rtc_*`)  | **GA** (Zephyr `rtc_*`)   | **GA** (Zephyr `rtc_*`)   | code complete¹     | code complete¹       | code complete¹     |
| **Watchdog** (`<alp/wdt.h>`) | **GA** (Zephyr `wdt_*`) | **GA** (Zephyr `wdt_*`) | **GA** (Zephyr `wdt_*`)   | **GA** (Zephyr `wdt_*`)   | code complete¹     | code complete¹       | code complete¹     |
| **Audio** (`<alp/audio.h>`) | surface declared (impl v0.2) | surface declared (impl v0.2) | surface declared (impl v0.2) | surface declared (impl v0.2) | stub | stub | stub |
| **Camera** (`<alp/camera.h>`) | planned              | planned                  | planned                   | planned                   | stub               | stub                 | stub               |
| **IoT** (`<alp/iot.h>`)   | **GA** (Wi-Fi+MQTT)      | **GA** (Wi-Fi+MQTT)      | **GA** (Wi-Fi+MQTT)       | **GA** (Wi-Fi+MQTT)       | stub               | stub                 | stub               |

¹ **code complete** — migrated to the registry/dispatcher pattern with real Linux
backends in the v0.8 cycle (issue #33), which also lands the per-class
`alp_<class>_capabilities()` getter on Yocto:
RTC (`/dev/rtcN`), Watchdog (`/dev/watchdogN`), CAN (SocketCAN), PWM (`/sys/class/pwm`),
ADC (IIO sysfs), Counter (Linux Counter sysfs), I²S + Audio (ALSA `snd_pcm_*` — gated on
`libasound`, fall back to the stub when absent), RPC (OpenAMP/RPMsg userland — gated
on `open-amp`/`libmetal`, with a NOSUPPORT fallback), MQTT (libmosquitto — gated on
`libmosquitto`), and Security (OpenSSL `EVP_*` — gated on `libssl`/`libcrypto`); classes
whose gating lib is absent degrade to the priority-0 `sw_fallback` backend.  Wi-Fi + BLE
also gained dispatchers + `sw_fallback` on Yocto (closing the
declared-but-undefined `alp_wifi_capabilities` link gap) but deliberately ship **no**
real Linux backend — radio bring-up is a system-config concern (wpa_supplicant /
NetworkManager / BlueZ) and apps consume the resulting network through plain sockets.
Each backend compiles + passes an nm symbol-ownership audit where its gating lib exists
on the build host, but the **full Yocto link + on-target run are HIL-gated** (no
sysroot / real device nodes in CI; the mqtt/security backends additionally lack
`libmosquitto`/OpenSSL dev headers on the CI host, so their real paths are
compile-verified only where those are installed).  The cross-core
RPMsg proxy is a separate slice.

### Cross-cutting v0.2 capability infrastructure

| Capability                                      | Status | Notes |
|-------------------------------------------------|--------|-------|
| `alp_last_error()` thread-local                 | **GA** (Zephyr) | Stamped by every `*_open` failure path.  See ADR 0002. |
| `<alp/soc_caps.h>` generated capability tables  | **GA** | From `metadata/socs/**.json` via `gen_soc_caps.py`.  Selected by `CONFIG_ALP_SOC_<TOKEN>`. |
| `E1M_<CLASS>_COUNT` portability bounds      | **GA** | Cross-SoM-portable instance count per class.  See ADR 0004. |
| ABI snapshot diff tool                          | **GA** | `scripts/abi_snapshot.py --diff prior.json`. |
| `pr-generated-files.yml` CI workflow            | **GA** | Catches stale `soc_caps.h` / ABI snapshot. |

## v0.3.0 — declared (surfaces ship in v0.1, impl arrives in v0.3)

| Library              | Surface | Backed-by plan |
|----------------------|---------|----------------|
| BLE (`<alp/ble.h>`)  | declared| Zephyr `bt` host stack |
| Security (`<alp/security.h>`) | declared | MbedTLS PSA + per-SoC HW accelerator routing |
| MProc (`<alp/mproc.h>`) | declared | Zephyr `mbox_*` — the AEN MHU is now backed by alp-sdk's `alif,mhuv2-mbox` driver (AEN801, bench-unverified, #45/#50) — + `hwsem_*` + shared-memory regions |

## v0.4.0 prep — landed on `main` (2026-05-11)

Yocto-side surface advances ahead of the v0.4 tag.  Each item is
gated by either a CMake `find_package` / `pkg_check_modules` check
or a default-disabled `west.yml` group, so workspaces that don't
need v0.4 fall back cleanly to the v0.3 state above.

### Cortex-A (Yocto)

| Library                              | AEN E5..E8: a32_cluster Yocto | V2N: a55_cluster Yocto | V2N-M1: a55_cluster Yocto | iMX93: a55_cluster Yocto |
|--------------------------------------|-------------------------------|------------------------|---------------------------|--------------------------|
| **Peripherals (I2C)** (`<alp/peripheral.h>`) | code complete (untested) — i2c-dev | code complete (untested) — i2c-dev | code complete (untested) — i2c-dev | planned |
| **Peripherals (SPI)** (`<alp/peripheral.h>`) | code complete (untested) — spidev | code complete (untested) — spidev | code complete (untested) — spidev | planned |
| **Peripherals (UART)** (`<alp/peripheral.h>`)| code complete (untested) — termios | code complete (untested) — termios | code complete (untested) — termios | planned |
| **Peripherals (GPIO + IRQ)** (`<alp/peripheral.h>`) | code complete (untested) — chardev v2 + pthread `poll()` | code complete (untested) — chardev v2 + pthread `poll()` | code complete (untested) | planned |
| **Peripherals (UART RX ringbuf)** (`<alp/peripheral.h>`) | n/a (Linux kernel already buffers) | n/a (Linux kernel already buffers) | n/a | n/a |
| **IoT — MQTT cleartext** (`<alp/iot.h>`) | code complete (untested) — libmosquitto | code complete (untested) — libmosquitto | code complete (untested) — libmosquitto | planned |
| **IoT — MQTT TLS** (`mqtts://`)      | code complete (untested) — mosquitto_tls_set + system / pinned CA | code complete (untested) — mosquitto_tls_set + system / pinned CA | code complete (untested) | planned |
| **IoT — Wi-Fi station** (`<alp/iot.h>`) | sw_fallback by design (system-config via wpa_supplicant/NM) | sw_fallback by design (system-config via wpa_supplicant/NM) | sw_fallback by design | planned |
| **Audio** (`<alp/audio.h>`)          | code complete (untested) — ALSA `snd_pcm_*` | code complete (untested) — ALSA `snd_pcm_*` | code complete (untested) | planned |
| **Security** (`<alp/security.h>`)    | code complete (KATs green; meta-alp-sdk build mechanics verified 2026-05-26, full bake pending) — OpenSSL `EVP_*` | code complete (KATs green; meta-alp-sdk build mechanics verified 2026-05-26, full bake pending) — OpenSSL `EVP_*` | code complete (KATs green) | planned |
| **Mender OTA (meta-alp-sdk opt-in)**     | code complete (untested) — `require conf/distro/include/mender.inc` | code complete (untested) — `require conf/distro/include/mender.inc` | code complete (untested) | planned |

### Cortex-M (Zephyr)

| Library                              | AEN E3/E4: m55_hp Zephyr | AEN E3/E4: m55_he Zephyr | AEN E5..E8: m55_hp Zephyr | AEN E5..E8: m55_he Zephyr | V2N: m33_sm Zephyr | V2N-M1: m33_sm Zephyr | iMX93: m33 Zephyr |
|--------------------------------------|--------------------------|--------------------------|---------------------------|---------------------------|--------------------|----------------------|--------------------|
| **Peripherals (I2C)** (`<alp/peripheral.h>`) | **GA**          | **GA**                   | **GA**                    | **GA**                    | stub               | stub                 | stub               |
| **Peripherals (SPI)** (`<alp/peripheral.h>`) | **GA**          | **GA**                   | **GA**                    | **GA**                    | stub               | stub                 | stub               |
| **Peripherals (UART)** (`<alp/peripheral.h>`)| **GA**          | **GA**                   | **GA**                    | **GA**                    | stub               | stub                 | stub               |
| **Peripherals (GPIO + IRQ)** (`<alp/peripheral.h>`) | **GA**   | **GA**                   | **GA**                    | **GA**                    | stub               | stub                 | stub               |
| **Peripherals (UART RX ringbuf)** (`<alp/peripheral.h>`) | code complete (untested) — LwRB-backed IRQ drain | code complete (untested) — LwRB-backed IRQ drain | code complete (untested) | code complete (untested) | stub | stub | stub |
| **IoT — MQTT cleartext** (`<alp/iot.h>`) | planned (Zephyr `mqtt_*`) | planned (Zephyr `mqtt_*`) | planned (Zephyr `mqtt_*`) | planned (Zephyr `mqtt_*`) | stub | stub | stub |
| **IoT — MQTT TLS** (`mqtts://`)      | planned                  | planned                  | planned                   | planned                   | stub               | stub                 | stub               |
| **IoT — Wi-Fi station** (`<alp/iot.h>`) | planned               | planned                  | planned                   | planned                   | stub               | stub                 | stub               |
| **Audio** (`<alp/audio.h>`)          | surface declared (impl v0.2) | surface declared (impl v0.2) | surface declared (impl v0.2) | surface declared (impl v0.2) | stub | stub | stub |
| **Security** (`<alp/security.h>`)    | surface declared (impl v0.3) | surface declared (impl v0.3) | surface declared (impl v0.3) | surface declared (impl v0.3) | stub | stub | stub |
| **mproc IPC framing** (`<alp/mproc.h>`) | code complete (untested) — placeholder 12-byte envelope; replaced by nanopb-generated codec in v0.4-final | code complete (untested) — placeholder 12-byte envelope | code complete (untested) | code complete (untested) | stub | stub | stub |
| **MCUboot secure-boot scaffolding**  | sysbuild profile + dev-key generator + `docs/secure-boot.md` (compile-verification gates on `alp_e1m_evk_aen` board file) | sysbuild profile + dev-key generator + `docs/secure-boot.md` | sysbuild profile + dev-key generator + `docs/secure-boot.md` | sysbuild profile + dev-key generator + `docs/secure-boot.md` | stub | stub | stub |
| **Mender OTA (meta-alp-sdk opt-in)**     | doc-only (`mender-mcu-client` vs Hawkbit decision pending) | doc-only | doc-only | doc-only | n/a | n/a | n/a |

The Yocto MQTT / audio / security backends are each conditional on
their own `pkg_check_modules` check (`libmosquitto`, `alsa`,
`libssl libcrypto`); since the #33 registry migration, workspaces
without the matching `-dev` package on the sysroot degrade to the
priority-0 `sw_fallback` backend of the class (not the old
stub_backend.c NOSUPPORT stubs).  Per-class
`ALP_VENDOR_OVERRIDES_<CLASS>` macros in
`src/common/stub_backend.c` let each surface roll out independently
across backends -- the currently-defined class gates include `I2C`,
`SPI`, `UART`, `GPIO`, `MQTT`, `AUDIO_IN`, `AUDIO_OUT`, `SECURITY`,
`WIFI`, `BLE`, `CAN`, `PWM`, `ADC`, `I2S`, `COUNTER`, `RTC`, `WDT`,
and `UART_RX_RINGBUF`.

## v0.5+ surfaces (listed for completeness — surface-only / untested)

These `<alp/*>` surfaces landed (public header + a backend or SW
fallback) after the v0.4-prep cut.  They are listed here so the matrix
is complete; **almost none has per-SoM HIL verification yet** (the DAC
row's E8 bench pass is the exception) — treat every row
as surface-only / untested until the matching [`test-plan.md`](test-plan.md)
row flips.  They are deliberately **not** broken out per-core × per-SoM:
asserting a status in each of the 11 cells would overclaim coverage that
hasn't been measured.

| Surface | Header(s) | Cores / backing | Status |
|---------|-----------|-----------------|--------|
| Display class | `display.h` | M (Zephyr `display_*` driver-class wrapper, `alp-display0..3` DT aliases, issue #23); A (Yocto) + baremetal: NOSUPPORT stub | Zephyr backend **code complete (untested on silicon)** — native_sim ZTESTs against the upstream dummy display cover open/get_caps/blit/clear/close + degrade paths; no panel has been driven on real hardware through this class yet.  (The v0.1 Display "GA (SSD1306)" rows are the **chip-driver + LVGL re-export path**, not this class.)  V2N DSI / parallel-RGB + Alif LCD-IF vendor backends still pending |
| Inference dispatcher | `inference.h` + `backend.h` | M (Zephyr): registry over `tflm` / `ethos_u`; A (Yocto): dispatcher over `tflm` / `drpai` / `deepx_dxm1` | surface + registry present; the A55 **DeepX (`dxrt::InferenceEngine`)** + **DRP-AI (`MeraDrpRuntimeWrapper`)** backend bodies are **real, bench-unverified** (link needs the Yocto sysroot; default-off CMake options); the former M-class DRP-AI/DEEPX stubs are removed — both engines are A55-only, M-class runs TFLM (code-complete) — #58/#59; `tflm`/`ethos_u` paths still untested |
| DSP / math offload | `dsp.h` + `tmu.h` | M + A; CMSIS-DSP / libm SW fallback, GD32 FAC/CORDIC HW path on V2N | surface present; **untested** on HW |
| Storage | `storage.h` | M (LittleFS) + A (filesystem) | surface present; **untested** |
| 2D graphics | `gpu2d.h` | portable **software fallback** (real, native_sim **unit-tested**) + Alif **D/AVE 2D** backend (real, bench-unverified) | sw_fallback `fill_rect`/`blit`/`blend` exact-pixel ZTESTs pass on native_sim + **E8 bench PASS** (RAM-run, 2026-06-17); D/AVE 2D code-complete, bench-unverified (ADDITIVE/MULTIPLY blends delegate to the sw path).  (AEN 2D engine is **D/AVE 2D** (TES D/AVE 2D), not Mali-D71; i.MX 93 = **PXP**, no Vivante — N93 is served by the sw fallback today, now wired on `ALP_OS=yocto` plain-CMake builds too (dispatcher + sw_fallback replace the NOSUPPORT stub, ctest-covered); a Linux-side PXP/`libg2d` backend is future work gated on the `meta-imx` machine wiring + an in-repo API source, see #24) |
| Power management | `power.h` | M (Zephyr `pm_*`) + A | surface present; **untested** |
| Heterogeneous RPC | `rpc.h` (+ generated `system_ipc.h`) | A↔M over RPMsg / OpenAMP | surface + scaffold; **untested** |
| DAC | `dac.h` (split out of `adc.h` in v0.8) | M (Zephyr `dac_*`) + A (Yocto registry backend, issue #33) | Zephyr backend real — **E8 bench PASS** (`dac_alif`, v0.8.0 campaign); Yocto code-complete, HIL-gated; `alp_dac_capabilities()` additive in v0.9 (conformance-suite covered on native_sim) |
| I²C/SPI target (slave) mode | `peripheral.h` (`alp_i2c_target_*` / `alp_spi_target_*`, v0.9, `[ABI-EXPERIMENTAL]`) | M (Zephyr `i2c_target_register` / `SPI_OP_MODE_SLAVE`); Yocto + baremetal: NOSUPPORT stubs (no Linux slave-mode uAPI) | Zephyr backend real; `alp_spi_target_transceive` takes a `timeout_ms` bound (finite timeouts need `CONFIG_SPI_ASYNC` — sync-only builds answer `ALP_ERR_NOSUPPORT`) and `alp_spi_target_close` refuses `ALP_ERR_BUSY` while a transfer is in flight; drivers without target support degrade with `ALP_ERR_NOSUPPORT`; native_sim covers param-validation + degrade paths — **two-board HIL pending** |
| SDK lifecycle | `peripheral.h` (`alp_init` / `alp_deinit`, v0.9) | all OSes (thin, idempotent) | present; every `peripheral-io` example calls it first |
| SoC identity | `hw_info.h` (`alp_soc_info_read` / `alp_soc_secure_fw_ping`, v0.9, `[ABI-EXPERIMENTAL]`) | M (Alif SE-service backend on AEN); elsewhere the SW fallback answers `soc_ref` only + NOSUPPORT | surface + AEN SE backend; **bench-gated** |
| Power profiles (operating points) | `power.h` (`alp_power_profile_get/_set`, v0.9, `[ABI-EXPERIMENTAL]`) | M (Alif SE-service backend on AEN); NOSUPPORT elsewhere | surface + AEN SE backend; **bench-gated** (set() is brown-out-capable — treat like a firmware update) |
| Peer-core boot | `mproc.h` (`alp_mproc_boot_core`, v0.9, `[ABI-EXPERIMENTAL]`) | M (Alif SE-service boot authority on AEN); NOSUPPORT where the platform boots peers by other means | surface + AEN SE backend; **bench-gated** |
| SDK version / ABI feature-test | `version.h` (v0.9, `[ABI-STABLE]`) | all OSes (compile-time macros + `alp_version_string()`) | present; value-sync CI-gated (`check_version_doc_sync.py`) |
| Update audit log | `update_log.h` (v0.7, `[ABI-EXPERIMENTAL]`) | M (Zephyr only today): hash-chain engine + SW tier — **NVS-persistent** when the board carves an `alp_ulog_partition` (`CONFIG_ALP_SDK_UPDATE_LOG_PERSIST`), RAM fallback otherwise; tamper-EVIDENT, not tamper-proof; Yocto/baremetal: not built | code complete (native_sim unit-tested: chain verdicts, persist-across-reinit, full-log NOMEM-no-wrap, RAM fallback, tier selection + degrade); **no on-silicon persistence proof yet** — #262 |
| Update audit log — HW_ENFORCED tier | `update_log.h` `ALP_UPDATE_LOG_HW_ENFORCED` (`CONFIG_ALP_SDK_UPDATE_LOG_TFM`) | M (TF-M builds only): app-immutable tier — store → PSA Protected Storage in the SPE, anchor → HW monotonic counter | **stub** — dispatcher-side plumbing + capability-gated registration + degrade landed; `ready()` returns NOSUPPORT so `alp_update_log_open()` falls through to the SW tier (native_sim-tested). Prerequisites for GA: `psa_ps_set`/`psa_ps_get` under `BUILD_WITH_TFM` + a HW monotonic-counter service (neither the pinned Zephyr nor hal_alif v2.2.0 SE — OTP-only — exposes a non-secure NV counter) — #111 |

## CMSIS-DSP per-SoM validation

Alp SDK does not re-export CMSIS-DSP -- application code includes
`arm_math.h` directly when DSP/math primitives are needed.  Alp's
own internals (e.g. filtering inside `<alp/audio.h>` ) optionally
pull in CMSIS-DSP when the build sets `ALP_HAS_CMSIS_DSP`.  Either
way the validated **feature groups** depend on each SoC's
instruction set extensions:

| Family         | Core(s)                  | DSP-ext | Helium (MVE) | Validated CMSIS-DSP groups (v0.1)               |
|----------------|--------------------------|---------|--------------|-------------------------------------------------|
| E1M-AEN        | Cortex-M55 + Ethos-U55   | yes     | yes          | Filtering, Transform, Matrix, Statistics, NN.   |
| E1M-X V2N      | Cortex-A55 + DRP-AI3     | n/a (A) | n/a (NEON)   | All scalar paths; NEON used where available.    |
| E1M-X V2N-M1   | Cortex-A55 + DRP-AI3 + M1| n/a (A) | n/a (NEON)   | Same as V2N (M1 is an external NPU).            |

Helium-accelerated paths are only enabled when the build sets
`-DALP_CMSIS_DSP_HELIUM=ON` and the toolchain confirms `__ARM_FEATURE_MVE`.

---

v0.6 introduces the per-core matrix shape; ADR 0010 explains why.
The "AEN / Zephyr" style columns prior to v0.6 collapsed M55-HP +
M55-HE into a single cell; the new shape tracks each core
separately because the SDK now builds each independently.
