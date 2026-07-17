# aen-cc3501e-bringup

Host (Alif) side of the on-module **TI CC3501E** Wi-Fi 6 + BLE 5.4
coprocessor bring-up on the **E1M-AEN801** (Alif Ensemble E8) SoM.

This app runs on the Alif **M55-HE** core. It powers the CC3501E (whose
supply is host-gated), resets it, and exercises the inter-chip control
link with `PING` / `GET_VERSION` / `GET_DIAG_INFO`, printing every step on
the carrier console. It is the AEN analog of
[`examples/v2n/v2n-gd32-bridge-ping`](../../v2n/v2n-gd32-bridge-ping) (the
GD32 supervisor bring-up) — same shape, different coprocessor.

The CC3501E peer firmware is ALP-authored and lives in this repo at
[`firmware/cc3501e/`](../../../firmware/cc3501e) (embedded, per ADR 0015 —
like the gd32-bridge).

## Why this app is the gating step

The CC3501E has **no power** until the Alif drives `WIFI_EN` high. A J-Link
attached to the CC3501E reads `VTref = 0 V` and cannot connect until then.
So running this app is what first powers the coprocessor and enables the
very first on-silicon `PING` — which also validates the firmware's
reply-arming framing (the one fix that can't be checked off-silicon).

## Wiring (`metadata/e1m_modules/aen/inter-chip.tsv`)

| Net          | Alif pad     | Dir | CC3501E pad             |
|--------------|--------------|-----|-------------------------|
| `WIFI_EN`    | `P15_5`      | out | supply gate             |
| `E_WIFI.NRST`| `P15_1_FLEX` | out | reset                   |
| `SPI1.SCK`   | `P14_6`      | out | `GPIO_27` (SPI0 slave)  |
| `SPI1.MOSI`  | `P14_5`      | out | `GPIO_28`               |
| `SPI1.MISO`  | `P14_4`      | in  | `GPIO_29`               |

This HW rev uses the dwc-ssi **hardware SS0** chip-select on P14_7 and a READY
input for per-phase gating. The application still passes `ALP_SPI_NO_CS` so no
software GPIO CS is installed; the SPI driver drives SS0 from the controller.

## What it does

1. Opens `WIFI_EN` + `E_WIFI.NRST` GPIOs (output).
2. Opens SPI1 (`bus_id = 1`, mode 0, 8 MHz, hardware SS0) — Alif is master.
3. `cc3501e_reset()` — sequences `WIFI_EN` + `nRESET` (TI SWRU626) and
   blocks ~905 ms for the boot budget.
4. Retries `PING` until the coprocessor answers.
5. `GET_VERSION` — checks the protocol version matches
   `ALP_CC3501E_PROTOCOL_VERSION`.
6. `GET_DIAG_INFO` — v2-firmware feature; v0.1 firmware rejects it cleanly
   (expected — it still proves the request round-trips).
7. Liveness soak: `PING` every 500 ms (+ `GET_VERSION` every 8th cycle) so
   the link stays continuously verifiable over J-Link.

## Build + flash (bench)

Two J-Links: one on the Alif, one on the CC3501E.

```sh
# 1. Build + flash the CC3501E peer firmware first (see firmware/cc3501e):
#    powershell firmware/cc3501e/ti/build_ti.ps1   (after repinning the
#    SysConfig board file to the AEN GPIO_27/28/29 inter-chip pins)
#    -> flash cc3501e-bridge.hex over the CC3501E J-Link.
#    NOTE: the CC3501E reads VTref=0V until this app powers it, so flash it
#    with the Alif app already powering WIFI_EN, or hold WIFI_EN externally.

# 2. Build + flash this Alif app (use the FULL qualified board target so the
#    per-board overlay boards/<target>.overlay is auto-applied):
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-bringup
west flash                      # over the Alif J-Link (AE822FA0E5597LS0_M55_HE)
```

> The per-board overlay is named for the **full** board target
> (`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`); Zephyr's
> auto-discovery matches the qualified target, not the bare board name.

Watch the carrier console (Alif **UART5**, P3_4/P3_5, 115200 8N1 — the E1M
edge "UART0") for the `[cc3501e-bringup]` log. If the link wedges, cold-cycle
via the bench PSU CH2.

## Builds verified (off-silicon)

- **native_sim/native/64** (`twister`, `build_only`) — against the emulated
  SPI controller + `alp,pin-array` in `boards/native_sim_native_64.overlay`.
- **alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he** (real M55-HE ARM target,
  Zephyr SDK) — produces a flashable `zephyr.elf` (~50 KB flash). This proves
  the AEN overlay DT, the `spi_dw_alif` (`alif,dwc-ssi-spi`) binding, the SPI1
  pinctrl macros, and the lpgpio enable all compile for silicon. The
  `orphan section alp_backends_*` linker warnings are by design (the backend
  registry uses C-identifier section names so GNU ld emits the
  `__start_/__stop_` boundary symbols — see `include/alp/backend.h`).

## Bench-unverified notes

`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` is a **bench artifact**, like the rest
of the alp-sdk Alif peripheral layer. Resolved against
`zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h`:

- **SPI1 pinctrl** = alt-2 with the `_C` suffix
  (`PIN_P14_6__SPI1_SCLK_C` / `_MOSI_C` / `_MISO_C`) — confirmed;
- **LP-GPIO mux** = reset default (`PIN_P15_x__LPGPIO` = alt-0), so no pinctrl
  group is needed for WIFI_EN / nRESET — confirmed.

The one remaining genuine unknown (upstream Zephyr declares no SPI node, so it
is not in the build tree):

- the **SPI1 IRQ** (`138` follows spi0's `137`) — confirm against the AE822
  TRM before relying on the interrupt-driven path on silicon.
