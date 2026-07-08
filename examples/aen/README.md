# AEN-specific examples

Reference applications for the E1M-AEN family (lead part: Alif
Ensemble E8 -- dual-M55 + Ethos-U85/U55 NPUs, on-module ISP /
camera path, GPU2D).  Build any of these against an E1M-AEN SoM
populated on the E1M-EVK board; the per-example `board.yaml`
carries the exact SKU + board.

Apps tagged **bench** are on-silicon RAM-run validations read over SWD via the
RAM console; **regcheck/staging** apps prove a driver binds + programs its
registers (often bind-only where external wiring is HW-blocked); the rest are
full reference applications over the portable `<alp/*>` or standard Zephyr APIs.

### Flagship + system

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`edgeai-vision-aen`](edgeai-vision-aen/)          | End-to-end EdgeAI vision pipeline -- CSI camera -> ISP Pico -> Ethos-U55 inference -> OLED overlay. The flagship AEN demo. |
| [`aen-mcuboot-smoke`](aen-mcuboot-smoke/)          | **bench** -- production secure-boot chain `SES -> MCUboot -> slot0 -> app` on the E8 (SE root-of-trust, `sysbuild/aen`); RESULT PASS end-to-end. |
| [`aen-hp-core-smoke`](aen-hp-core-smoke/)          | **bench** -- first light on the second M55 (RTSS-HP); every other AEN app runs on the M55-HE. |
| [`aen-power-smoke`](aen-power-smoke/)              | **bench** -- Stage-A low-power smoke: architectural Cortex-M55 WFI sleep + wake, no PM subsystem. |
| [`aen-power-iwic`](aen-power-iwic/)                | **bench** -- Stage-B IWIC deep-sleep over the Alif PM layer on pinned Zephyr 4.4.0. |
| [`aen-aipm-read`](aen-aipm-read/)                  | **bench** -- READ-ONLY dump of the live RUN/STANDBY operating-point profiles via the portable `alp_power_profile_get()` (SE aiPM-backed on AEN). |
| [`aen-wdt-feed`](aen-wdt-feed/)                    | CMSDK watchdog install + feed over the upstream `arm,cmsdk-watchdog` driver (Tier-1). |

### Dual-core (B1) + IPC

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-dualcore-probe`](aen-dualcore-probe/)        | **bench** -- the decisive B1 test: does a dual-entry ATOC boot BOTH M55 cores? Heartbeat stamps in global SRAM0. |
| [`aen-dualcore-master`](aen-dualcore-master/)      | **bench** -- the SES-booted core starts the other M55 at runtime via the portable `alp_mproc_boot_core()` (SE boot service over the `seservice0` MHU on AEN). |
| [`aen-dualcore-doorbell`](aen-dualcore-doorbell/)  | **bench** -- HE->HP MHU-1 doorbell with both M55 cores live (the completion of B1). |
| [`aen-dualcore-ipc`](aen-dualcore-ipc/)            | **bench** -- bidirectional HE<->HP shared-memory request/response over a global-SRAM0 mailbox. |
| [`aen-rpc-pingpong`](aen-rpc-pingpong/)            | OpenAMP RPMsg (`ipc_service`) ping/pong between the two M55 cores over the `alif,mhuv2-mbox` MBOX driver. |
| [`aen-alp-rpc`](aen-alp-rpc/)                       | The same HP<->HE ping/pong via the portable `<alp/rpc.h>` surface instead of the raw Zephyr API. |

### Secure Enclave + security

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-se-service-info`](aen-se-service-info/)      | **staging** -- SE SERVICE transport binds + a single LCS read over the bench RAM-run flow (deliberately vendor-specific bring-up regcheck; customer code uses the portable wrappers instead). |
| [`aen-se-service-query`](aen-se-service-query/)    | **bench** -- READ-ONLY dump of the portable SE-backed surfaces: SoC identity (`<alp/hw_info.h>`), RUN/STANDBY profiles (`<alp/power.h>`), TRNG (`<alp/security.h>`). |
| [`aen-se-crypto`](aen-se-crypto/)                  | SHA-256 known-answer + AES-128-GCM round-trip + TRNG through `<alp/security.h>`, backed by the SE CryptoCell (else MbedTLS-PSA fallback). |
| [`aen-secure-element-sign`](aen-secure-element-sign/) | OPTIGA Trust M I2C_STATE probe over BRD_I2C (LPI2C0, M55-HE); product-info/raw-APDU return `ALP_ERR_NOSUPPORT` until host-library integration. |

### NPU + AI accelerators

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-npu-ethosu-regcheck`](aen-npu-ethosu-regcheck/)     | **regcheck** -- presence + clock probe of the global Arm Ethos-U85 NPU (`arm,ethos-u`, ethosu85 @ 0x49042000). |
| [`aen-npu-ethosu55-regcheck`](aen-npu-ethosu55-regcheck/) | **regcheck** -- presence + clock probe of the HE-core-local Arm Ethos-U55 (128-MAC, core-private 0x400e1000). |
| [`aen-npu-inference`](aen-npu-inference/)          | **bench** -- first on-silicon NPU inference: a fused subgraph dispatched to the Ethos-U85 (256-MAC) via the Arm core driver. |
| [`aen-npu-inference-alif`](aen-npu-inference-alif/)| **bench** -- matched-runtime Vela int8 inference end-to-end via the Arm `InferenceProcess` wrapper -> TFLM; hermetic fixture. |
| [`aen-npu-inference-person-mram`](aen-npu-inference-person-mram/) | **bench** -- the real `person_detect` MobileNet (int8) end-to-end with the model resident in MRAM slot0 (production-scale). |
| [`aen-gpu2d-bench`](aen-gpu2d-bench/)              | **bench** -- GPU2D software-fallback validation on the M55-HE. |

### Camera / display / media

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-camera-regcheck`](aen-camera-regcheck/)      | **staging** -- camera-capture stack (`alif,cam` / DesignWare CSI-2 + D-PHY / ON Semi ARX3A0) binds; sensor not wired this batch. |
| [`aen-isp-regcheck`](aen-isp-regcheck/)            | **staging** -- VeriSilicon ISP Pico (`vsi,isp-pico`) bind-only staging check. |
| [`aen-dsi-regcheck`](aen-dsi-regcheck/)            | **staging** -- Alif C2-MIPI-DSI display stack bind-only check (the TX twin of the CSI camera path). |
| [`aen-i2s-amp-alif`](aen-i2s-amp-alif/)            | Drive a tone out of the E8 audio I2S (`i2s3`) over the vendored DesignWare I2S driver (Zephyr I2S API). |
| [`aen-pdm-mic-alif`](aen-pdm-mic-alif/)            | Capture PCM from the EVK's PDM mics (MP34DT05) through the HP PDM block + the vendored `alif,alif-pdm` DMIC driver (Zephyr DMIC API). |

### Connectivity + storage

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-ethernet-link`](aen-ethernet-link/)          | Bring up the E8 GMAC (`eth_dwmac` + the `alif,ethernet` glue) and report net-if state (Zephyr net-if API). |
| [`aen-cc3501e-bringup`](aen-cc3501e-bringup/)      | Host (Alif M55-HE) side of the on-module TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor bring-up (power-gate, reset, control link). |
| [`aen-cc3501e-companion-tour`](aen-cc3501e-companion-tour/) | **capstone** -- full-surface tour of the CC3501E companion API: init -> ping -> diag -> Wi-Fi scan/connect/IP -> TCP socket -> BLE enable/scan -> proxied-GPIO read. |
| [`aen-can-regcheck`](aen-can-regcheck/)            | **staging** -- Alif CAN-FD controller bind-only staging check (bus wiring HW-blocked). |
| [`aen-sdcard-readout`](aen-sdcard-readout/)        | Probe a microSD over the E8 SD Host Controller (`snps,dwc-sdhc` + SDMMC disk) via the disk-access API. |
| [`aen-spi-regcheck`](aen-spi-regcheck/)            | **bench** -- Alif DWC_ssi SPI driver (`alif,dwc-ssi-spi`, spi0 @ 0x48103000) register validation. |
| [`aen-uart-ns16550-loopback`](aen-uart-ns16550-loopback/) | **bench** -- upstream ns16550 UART (Alif uart3 @ 0x4901b000) loopback validation. |

### EEPROM / I2C / GPIO / bus peripherals

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-eeprom-manifest`](aen-eeprom-manifest/)      | Read + decode the on-module 24C128 Alp hardware-info manifest over SoC I2C2 (upstream `i2c_dw`) via `<alp/*>`. |
| [`aen-i2c2-eeprom-regcheck`](aen-i2c2-eeprom-regcheck/) | **regcheck** -- I2C2 + EEPROM over the upstream DesignWare `i2c_dw` driver (Tier-1, `snps,designware-i2c`). |
| [`aen-gpio-bench`](aen-gpio-bench/)                | **bench** -- GPIO over the upstream DesignWare `gpio_dw` driver (Tier-1, `snps,designware-gpio`). |
| [`aen-dma-regcheck`](aen-dma-regcheck/)            | **bench** -- ARM PL330 DMA (`arm,dma-pl330`, Tier-1): a real memory-to-memory copy through RTSS-HE local DMA2 in global SRAM0. |
| [`aen-hwsem-regcheck`](aen-hwsem-regcheck/)        | **regcheck** -- HWSEM instance 0 (`alif,hwsem`) take/give readout over the in-tree driver. |
| [`aen-crc-regcheck`](aen-crc-regcheck/)            | **regcheck** -- hardware CRC engine (`alif,crc`) known-value check via the Zephyr CRC class API. |

### Analog (ADC / DAC / comparator)

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-adc-regcheck`](aen-adc-regcheck/)            | **regcheck** -- Alif ADC driver (`alif,adc`, adc12_0 @ 0x49020000) register validation (no analog path). |
| [`aen-dac-regcheck`](aen-dac-regcheck/)            | **regcheck** -- Alif DAC driver (`alif,dac`, dac0 @ 0x49028000) register validation (no analog path). |
| [`aen-cmp-regcheck`](aen-cmp-regcheck/)            | **regcheck** -- HSCMP analog comparator (`alif,cmp`, cmp0 @ 0x49023000) internal-reference smoke. |
| [`aen-analog-validate`](aen-analog-validate/)      | **bench** -- real DAC0 -> ADC analog loopback (proves the analog path, not just registers). |

### Timers / counters / PWM / encoder

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-counter-utimer-regcheck`](aen-counter-utimer-regcheck/) | **regcheck** -- UTIMER free-running counter advances (Tier-1.5 `alif,utimer-counter`). |
| [`aen-counter-alarm-regcheck`](aen-counter-alarm-regcheck/)   | **regcheck** -- UTIMER COMPARE-A one-shot alarm callback fires (sibling of the counter check). |
| [`aen-lptimer-regcheck`](aen-lptimer-regcheck/)    | **regcheck** -- always-on LPTIMER (`alif,lptimer`, lptimer @ 0x42001000) counter readout via the `counter_*` API. |
| [`aen-pwm-utimer-pwmleds`](aen-pwm-utimer-pwmleds/)| **regcheck** -- Alif UTIMER PWM driver (consumer-node `pwm-leds` variant). |
| [`aen-qenc-readout`](aen-qenc-readout/)            | Read a quadrature encoder on the E8 UTIMER via the `alif,utimer-qdec` sensor driver (Zephyr sensor API). |

### RTC

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`aen-rtc-regcheck`](aen-rtc-regcheck/)            | **regcheck** -- always-on LPRTC (`snps,dw-apb-rtc`, lprtc @ 0x42000000) counter readout via the `counter_*` API. |
| [`aen-rtc-calendar`](aen-rtc-calendar/)            | **bench** -- the portable `alp_rtc_set_time()` / `alp_rtc_get_time()` calendar surface over the LPRTC counter (shim). |

## Why a separate index here

The top-level [`examples/README.md`](../README.md) lists every
example.  This sub-index exists because AEN-specific examples
need an AEN-family SoM (no fallback path on V2N / N93) -- having
them in their own directory makes that constraint visible from
the filesystem layout alone.  Cross-family examples (gpio,
i2c, pwm, ...) stay at the top level of `examples/`.

## See also

- [`docs/soms/aen.md`](../../docs/soms/aen.md) -- AEN SoM
  one-pager + supported peripherals.
- [`docs/getting-started.md`](../../docs/getting-started.md) --
  EVK bring-up flow that lands you ready to run these examples.
- AEN feature coverage matrix -- maintained in the
  internal `alp-sdk-internal` repo as `AEN-FEATURE-AUDIT-2026-05.md`
  (what the SDK exposes vs the silicon's full surface).
