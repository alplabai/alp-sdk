# OS Support Matrix

Status keys:
- **GA** — implemented, tested on real silicon, in CI.
- **stub** — header surface compiles; functions return `ALP_ERR_NOSUPPORT`.
- **planned** — declared roadmap, no code yet.
- **n/a** — combination not targeted.

## v0.1.0

| Library     | E1M-AEN / Bare-metal | E1M-AEN / Zephyr | E1M-X-V2N / Yocto | E1M-X-V2N-M1 / Yocto |
|-------------|----------------------|------------------|-------------------|----------------------|
| Peripherals | stub                 | **GA**           | stub              | stub                 |
| Display     | n/a                  | **GA** (SSD1306) | stub              | stub                 |
| Camera      | n/a                  | stub             | stub (planned v0.2 MIPI CSI-2) | stub (planned v0.2) |
| GUI/LVGL    | n/a                  | **GA** (re-export) | planned         | planned              |
| Math        | **GA** (re-export)   | **GA** (re-export) | **GA**          | **GA**               |
| Signal      | stub                 | stub             | stub              | stub                 |
| IoT         | n/a                  | **GA** (Wi-Fi+MQTT) | **GA** (Wi-Fi+MQTT) | **GA**          |

## v0.2.0 (target)

| Library     | E1M-AEN / Bare-metal | E1M-AEN / Zephyr | E1M-X-V2N / Yocto | E1M-X-V2N-M1 / Yocto |
|-------------|----------------------|------------------|-------------------|----------------------|
| Camera      | n/a                  | planned          | **GA** (MIPI CSI-2) | **GA**             |
| Signal      | **GA** (audio)       | **GA**           | **GA**            | **GA**               |

## CMSIS-DSP per-SoM validation

`alp/math.h` re-exports CMSIS-DSP unchanged; the validated **feature
groups** depend on each SoC's instruction set extensions:

| SoM          | Core(s)                  | DSP-ext | Helium (MVE) | Validated CMSIS-DSP groups (v0.1)               |
|--------------|--------------------------|---------|--------------|-------------------------------------------------|
| E1M-AEN      | Cortex-M55 + Ethos-U55   | yes     | yes          | Filtering, Transform, Matrix, Statistics, NN.   |
| E1M-X-V2N    | Cortex-A55 + DRP-AI3     | n/a (A) | n/a (NEON)   | All scalar paths; NEON used where available.    |
| E1M-X-V2N-M1 | Cortex-A55 + DRP-AI3 + M1| n/a (A) | n/a (NEON)   | Same as V2N (M1 is an external NPU).            |

Helium-accelerated paths are only enabled when the build sets
`-DALP_CMSIS_DSP_HELIUM=ON` and the toolchain confirms `__ARM_FEATURE_MVE`.
