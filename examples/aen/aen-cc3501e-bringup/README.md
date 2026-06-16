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

This HW rev wires **no chip-select and no host-IRQ** line. The host driver
clocks the protocol as deterministic fixed-count lockstep transfers; see
[`chips/cc3501e/cc3501e.c`](../../../chips/cc3501e/cc3501e.c).

## What it does

1. Opens `WIFI_EN` + `E_WIFI.NRST` GPIOs (output).
2. Opens SPI1 (`bus_id = 1`, mode 0, 8 MHz, no CS) — Alif is master.
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

# 2. Build + flash this Alif app:
west build -b alp_e1m_aen801_m55_he examples/aen/aen-cc3501e-bringup
west flash                      # over the Alif J-Link
```

Watch the carrier console (Alif **UART5**, P3_4/P3_5, 115200 8N1 — the E1M
edge "UART0") for the `[cc3501e-bringup]` log. If the link wedges, cold-cycle
via the bench PSU CH2.

## Off-silicon (CI)

`twister` builds this for `native_sim/native/64` (`build_only`) against the
emulated SPI controller + `alp,pin-array` in
`boards/native_sim_native_64.overlay`.

## Bench-unverified notes

The `boards/alp_e1m_aen801_m55_he.overlay` is a **bench artifact**, like the
rest of the alp-sdk Alif peripheral layer. Confirm before the first flash:

- the **SPI1 pinctrl alt-function** macro names
  (`PIN_P14_6__SPI1_SCLK` etc.) against
  `zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h` + the AE822 TRM
  pin-mux table — they may carry an `_A`/`_B`/`_C`/`_D` suffix;
- the **SPI1 IRQ** (`138` follows spi0's `137`, but is transcribe-TBD);
- that the FLEX LP-GPIO pads default to GPIO function (else add a pinctrl
  group selecting it).
