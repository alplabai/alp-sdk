# Cross-EVK Example Portability — Design

**Date:** 2026-05-24
**Status:** Approved design; pending implementation plan.
**Scope:** `examples/` only. Application firmware still targets a single
form factor — the E1M / E1M-X "separate product lines" principle is
unchanged for apps.

## Problem

Example sources are pinned to one form factor's pin namespace. The base
E1M demos `#include "alp/boards/alp_e1m_evk_routes.h"` and open pins via
`EVK_*` board macros (→ `E1M_*` pads); the E1M-X demos use `XEVK_*`
(→ `E1M_X_*` pads). The two namespaces are deliberately distinct, so a
demo written for one EVK will not build for the other — even when the
peripheral it exercises exists on both boards.

Per `alplabai/e1m-spec` `STANDARD.md` §7.2, most basic interfaces are
common to both form factors, so most peripheral demos *could* run on
either EVK if their source did not hard-code one board's macros.

## Goal

One example source builds and runs on **both** the E1M EVK and the
E1M-X EVK, with the target board chosen at **build time** (the standard
embedded model — you flash one board at a time). Pins resolve to the
selected board's pads. Demos that genuinely need a form-factor-specific
peripheral stay single-board and say so.

## Non-goals

- Runtime board detection / a single binary for both boards.
- Cross-form-factor portability for application firmware (apps still
  pick a form factor; this layer is an examples convenience).
- Exposing E1M-X-only peripherals through the portable layer.

## The common contract (e1m-spec §7.2)

The portable layer covers exactly the interfaces the spec lists with
matching availability on both form factors:

| Interface | E1M | E1M-X | In portable layer |
| --- | :---: | :---: | :---: |
| UART | 2 | 2 | UART0, UART1 |
| I²C | 2 | 4 | I2C0, I2C1 (I2C2/3 are X-only) |
| I³C | 1 | 1 | I3C0 |
| SPI | 2 | 3 | SPI0, SPI1 (SPI2 is X-only) |
| I²S | 2 | 2 | I2S0, I2S1 |
| CAN | 1 | 2 | CAN0 (CAN1 is X-only) |
| ADC | 8 | 8 | ADC0..7 |
| DAC | 2 | 2 | DAC0, DAC1 |
| Quadrature encoder | 4 | 4 | ENC0..3 |
| PWM | 8 | 8 | PWM0..7 |
| GPIO (default-function) | 23 | 34 | IO0..22 (IO23..33 are X-only) |
| USB 2.0 / 3.x | 1 / 1 | 1 / 2 | USB0 |
| GbE / SDIO / PDM / JTAG | yes | yes | as-is |

Notably DAC and ADC are common (§7.2: ADC 8/8, DAC 2/2). E1M (Alif
Ensemble) has a native 2-channel 12-bit DAC — `soc_caps.h`
`ALP_SOC_DAC_COUNT 2` on the Ensemble variants. On E1M-X the RZ/V2N SoC
has **no** native DAC (`ALP_SOC_DAC_COUNT 0`); the SoM provides the two
DAC channels through the on-module GD32G553 bridge. Both EVKs therefore
expose the full §7.2 common set, DAC included.

E1M-X-only interfaces (I2C2/3, SPI2, CAN1, the LCD0 24-bit parallel-RGB
class, GPIO IO23..33, the second USB3 / CSI / PCIe) get **no** portable
name. Using one in a cross-EVK example is a compile error — the
intended way the scheme flags a non-portable demo.

## Architecture

Three additive pieces. Nothing existing is removed; `E1M_*` / `E1M_X_*`
pinouts and `EVK_*` / `XEVK_*` board macros stay exactly as they are.

### 1. `BOARD_*` alias block (generated per board)

`scripts/gen_board_header.py` already emits
`include/alp/boards/alp_<board>_routes.h` from each
`metadata/boards/<board>.yaml`. It gains a new trailing section that
emits a **board-neutral `BOARD_*` alias** for every entry whose role is
part of the common contract:

```c
/* Portable cross-EVK aliases (e1m-spec §7.2 common set). */
#define BOARD_UART_DEBUG    EVK_UART_PORT_DEBUG   /* e1m-evk */
#define BOARD_I2C_SENSORS   EVK_I2C_BUS_SENSORS
#define BOARD_SPI_ARDUINO   EVK_SPI_BUS_ARDUINO
#define BOARD_I2S_AUDIO     EVK_I2S_AUDIO_CODEC
#define BOARD_CAN0          EVK_CAN_VEHICLE_BUS
#define BOARD_DAC0          EVK_DAC_ARDUINO_DAC0
#define BOARD_PIN_LED_RED   EVK_PIN_LED_RED
#define BOARD_ENC_ROTARY    EVK_ENC_ROTARY
/* ... */
```

The same `BOARD_*` names appear in `alp_e1m_x_evk_routes.h`, each
aliasing that board's `XEVK_*` macro. This is where the divergent
board-macro names are reconciled under one canonical name —
`BOARD_CAN0` is `EVK_CAN_VEHICLE_BUS` on the E1M EVK and `XEVK_CAN_BUS0`
on the E1M-X EVK; `BOARD_I2S_AUDIO` bridges `EVK_I2S_AUDIO_CODEC` and
`XEVK_I2S_AUDIO`; `BOARD_DAC0` bridges `EVK_DAC_ARDUINO_DAC0` and
`XEVK_DAC0`.

The `BOARD_*` role set is curated to the §7.2 common interfaces. Where a
board-specific silkscreen role does not map 1:1 across both boards
(e.g. the Arduino-header ADC channels differ), the portable layer
exposes the underlying common channel IDs (`BOARD_ADC0..7`) rather than
a board-specific header name. The role-to-`BOARD_*` mapping lives in a
small table in `gen_board_header.py` so both board headers stay in
lock-step and CI catches drift.

### 2. `<alp/board.h>` facade (board selection)

A new hand-written header `include/alp/board.h` selects the active
board's routes header at build time:

```c
#if   defined(ALP_BOARD_E1M_X_EVK)
#  include "alp/boards/alp_e1m_x_evk_routes.h"
#elif defined(ALP_BOARD_E1M_EVK)
#  include "alp/boards/alp_e1m_evk_routes.h"
#else
#  error "alp/board.h: no ALP_BOARD_* selected; set the board.yaml preset."
#endif
```

Cross-EVK examples include only `<alp/board.h>` and use only `BOARD_*`
names. They never name a form-factor pinout header or an `EVK_*` /
`XEVK_*` macro directly.

### 3. `ALP_BOARD_<SLUG>` build define

`scripts/alp_project.py` emits a compile define from the example's
`board.yaml` preset — `-DALP_BOARD_E1M_EVK` for `preset: e1m-evk`,
`-DALP_BOARD_E1M_X_EVK` for `preset: e1m-x-evk` (slug derived from the
preset name, mirroring `gen_board_header.py`'s `_board_slug`). This is
the single signal `<alp/board.h>` switches on. It is emitted for every
build path (Zephyr real-silicon, native_sim, plain-CMake) so the facade
resolves identically everywhere.

## Per-example portability class

Each example declares the board(s) it supports. `board.yaml` gains a
`supported_boards:` list (default: the single `preset:` value, so
existing single-board examples need no change):

```yaml
som:
  sku: E1M-AEN701
preset: e1m-evk            # the build-default preset
supported_boards:          # the EVKs this example is verified on
  - e1m-evk
  - e1m-x-evk
```

Three classes:

- **`both`** — uses only `BOARD_*` names + `<alp/board.h>`; lists both
  EVK presets.
- **`e1m-x-only`** — keeps `XEVK_*` / `E1M_X_*`; lists `e1m-x-evk`
  (e.g. anything using I2C2/3, SPI2, CAN1, LCD0, or a V2N-specific
  feature such as the GD32 bridge / eMMC / dual-Ethernet / DEEPX).
- **`e1m-only`** — keeps `EVK_*` / `E1M_*`; lists `e1m-evk`.

## The audit (verify, don't trust)

Every example's class is determined by checking the peripherals it uses
against §7.2 + `soc_caps.h` — **not** its comments. The motivating bug:
`dac-waveform`'s header comment claims "the base E1M doesn't route a
DAC; only E1M-X does," which is wrong — E1M has a native 2-channel DAC.
`dac-waveform` therefore reclassifies to `both`, switches
`E1M_X_DAC0` → `BOARD_DAC0`, and its comment is corrected. The
single-peripheral demos (uart, i2c, spi, pwm, adc, can, i2s, gpio,
qenc, rtc, dac) are expected to reclassify to `both`; camera/display/
PCIe-heavy and V2N-specific demos stay single-board.

## Error handling and edge cases

- **Non-portable peripheral in a `both` example** → the needed
  `BOARD_*` name does not exist → compile error naming the missing
  macro. This is the designed failure mode: it is impossible to ship a
  "both" example that silently depends on an X-only pad.
- **No `ALP_BOARD_*` define** → `#error` in `<alp/board.h>` (above), so
  a missing/typo preset fails loudly at compile time, not at link.
- **native_sim** → the board define is still emitted from the preset;
  `BOARD_*` resolves to integer instance IDs the emulated GPIO/bus
  drivers accept, so the same source compiles + runs under the emulator
  for each declared board.

## Testing / verification

native_sim, build-time, in CI (twister), at no hardware cost:

- A cross-EVK example's twister scenarios are generated **one per
  `supported_boards` entry**, each setting the matching `ALP_BOARD_*`
  define, all on `native_sim/native/64`. A `both` example thus produces
  two scenarios; both must pass. This is the CI proof that the same
  source compiles + runs under each board's `BOARD_*` resolution.
- Single-board examples produce one scenario, as today.
- A unit test (`tests/scripts/`) asserts that `alp_e1m_evk_routes.h` and
  `alp_e1m_x_evk_routes.h` define the **same** set of `BOARD_*` names
  (no portable name exists on only one board), and that
  `gen_board_header.py` regenerates both headers byte-identically (the
  generated-files gate).

## ABI / compatibility

Purely additive. `<alp/board.h>` and the `BOARD_*` aliases are new
public surface; every existing `E1M_*` / `E1M_X_*` instance ID and
`EVK_*` / `XEVK_*` board macro keeps its value and meaning. The ABI
snapshot grows by the new `BOARD_*` macros (additive, ABI-safe pre-1.0).

## Affected components

- `include/alp/board.h` — new facade (hand-written).
- `scripts/gen_board_header.py` — emit the `BOARD_*` alias block + the
  role→`BOARD_*` table; regenerates both `*_routes.h` headers.
- `scripts/alp_project.py` — emit `-DALP_BOARD_<SLUG>` from the preset.
- `metadata/schemas/board.schema.json` — allow `supported_boards:`.
- `examples/*/board.yaml` — add `supported_boards:` where cross-EVK.
- `examples/*/src/main.c` — reclassified `both` demos switch to
  `<alp/board.h>` + `BOARD_*`.
- `tests/scripts/` — the `BOARD_*` parity + per-board-scenario tests.
- `docs/` — board-macro / portability docs note the `BOARD_*` layer.

## Open risks

- The §7.2 common set is the *standard's* guarantee; a specific SoM
  could in principle under-populate an optional pad. The audit checks
  each example against `soc_caps` for the concrete SoMs the EVKs carry
  (Alif Ensemble on E1M, V2N on E1M-X), not just the standard, so a
  demo is only `both` if it builds for both real boards.
- The Arduino-header analog/PWM channel assignments differ between the
  EVKs; the portable layer exposes the common channel IDs, so a demo
  that depends on a *specific* Arduino silkscreen position stays
  single-board rather than being forced portable.
