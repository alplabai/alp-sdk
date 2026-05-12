# Hardware-in-the-Loop (HiL) rig — plan

> **Scope.** This document **describes the HiL rig the maintainer
> will build**.  No alp-sdk-side code lives in this doc -- only
> the requirements, expected test coverage, and the lattice of
> instruments that the rig combines.  Once the rig is on the
> bench, this doc gets re-purposed into the rig's user manual.

## Goal

A reproducible test harness that exercises a V2N (or V2N-M1) SoM
under fault injection so that ALP SDK + bridge firmware regressions
are caught **before** they ship to the field.

The rig is single-board (one SoM under test) -- multi-board
fanout is a follow-on once the single-board rig is proven.

## Hardware

| Block                     | Notes                                                                                                                  |
|---------------------------|------------------------------------------------------------------------------------------------------------------------|
| SoM-under-test            | `E1M-V2N101` or `E1M-V2M101` plugged into the `E1M-X-EVK` carrier.                                                     |
| HiL controller            | A separate workstation running the test orchestrator (Linux laptop or NUC).                                            |
| SWD probe                 | J-Link or ST-Link V3 -- attaches to both the Renesas SWD header (post-mortem only) and the GD32 SWD header (flash). |
| USB-UART                  | Console logging from `UART0` (Renesas).  Two ports captured concurrently: U-Boot and Linux journal.                    |
| Programmable load         | Resistive load on the E1M-edge 5 V rail so the rig can simulate carrier-side current draws (~0.5..2 A).               |
| Programmable supply       | 5 V bench supply with remote sense + voltage stepping so the rig can drop V_IN through brown-out range to exercise UVLO. |
| Logic analyser            | 16-channel, ~200 MS/s.  Captures GD32_SPI + BRD_I2C + Ethernet MDIO simultaneously.                                    |
| Ethernet link partner     | Managed switch with VLAN + per-port traffic generator (or a separate Linux box with `iperf3`).                          |
| Wi-Fi link partner        | Programmable AP for WPA2/3 + open networks; channel-hop scripting via OpenWrt or hostapd-cli.                         |
| BT link partner           | Linux box with BlueZ; programmable advertising / scanning patterns.                                                    |
| Cooling / thermal chamber | Optional phase-1 -- if the maintainer plans to test the `da9292_v2n_m1_enable_deepx_rail` thermal-warning path, a chamber that hits -10 / +85 °C is needed. |

## Test coverage map

The rig should be able to fire each of these on demand and capture
PASS/FAIL with a logic-analyser trace + UART log.

### Power tree

| Test                                                                       | Probe / instrument           |
|----------------------------------------------------------------------------|------------------------------|
| Cold-boot rail sequence matches `ACT88760_CMI120 Power Sequence_250916.pdf` | logic analyser + scope       |
| Brown-out below `VIN_UVLO`: PMIC drops, board safe-shutdown                  | programmable supply step     |
| `da9292_v2n_m1_enable_deepx_rail` succeeds within 5 ms                       | UART log + scope on CH2      |
| TPS628640 instances ACK on BRD_I2C (presence of all four addresses)         | logic analyser on BRD_I2C    |
| `act8760_get_status` reads no fault bits at steady state                     | UART log                     |
| Thermal-warning latches when chamber hits +85 °C                             | thermal chamber + UART log   |

### GD32 bridge

| Test                                                                                                | Probe / instrument          |
|-----------------------------------------------------------------------------------------------------|-----------------------------|
| `PING` round-trip on SPI fast path (CRC valid, status OK)                                            | logic analyser on SPI       |
| `PING` round-trip on I2C management path (CRC valid, status OK)                                      | logic analyser on BRD_I2C   |
| `GET_VERSION` returns the build-time major.minor.patch                                               | UART log                    |
| `GET_BUILD_ID` returns 20 ASCII bytes equal to the linker-emitted hash                                | UART log                    |
| `GPIO_READ` / `GPIO_WRITE` round-trip preserves bit values across all wired GD32 pads                | external GPIO probe         |
| `PWM_SET` / `PWM_GET` round-trip matches input frequency / duty within ±1 %                          | scope on PWM pad            |
| `ADC_READ` returns values within ±2 % of a precision reference (programmable supply driving the pad) | precision DAC + scope       |
| `DA9292_STATUS_FORWARD` cache age ≤ 20 ms (cache freshness)                                          | scope + bridge-firmware log |
| Corrupted CRC on either transport -> host receives `ALP_ERR_IO`; retry succeeds                       | logic analyser + UART log   |
| Wedged bridge firmware: host detects via missing `PING` reply and recovers via power-cycle           | UART log                    |

### Ethernet (per PHY, both ET0 + ET1)

| Test                                                                       | Probe / instrument      |
|----------------------------------------------------------------------------|-------------------------|
| MDIO probe reads PHYID1 = `0x001C` (Realtek OUI)                            | logic analyser on MDIO  |
| `soft_reset` self-clears within 500 ms                                      | UART log                |
| `restart_autoneg` + 1 Gb partner link-trains within 5 s                     | UART log + switch link  |
| `get_link` reports correct speed/duplex at 10 / 100 / 1000 Mb/s             | switch link             |
| `iperf3` 1 Gb test sustains > 900 Mb/s                                      | iperf3 + switch counters |
| WoL magic-packet detected after entering low-power state                    | switch unicast + UART   |
| Cable pull → `get_link` reports `up=false` within 1 s                        | manual + UART log       |

### Wi-Fi / BT

| Test                                                                       | Probe / instrument      |
|----------------------------------------------------------------------------|-------------------------|
| `WL_REG_ON` toggle brings `WL_HOST_WAKE` low; OS sees Wi-Fi device          | UART log + AP log       |
| Wi-Fi scan returns the programmable AP in ≤ 3 s                             | UART log                |
| WPA2 association succeeds in ≤ 5 s                                          | UART log                |
| `BT_REG_ON` toggle brings BT device up in BlueZ                             | UART log + linux box    |
| BLE advertisement / scan round-trip                                         | linux box BlueZ log     |

### DEEPX (V2N-M1 only)

| Test                                                                       | Probe / instrument                |
|----------------------------------------------------------------------------|-----------------------------------|
| `deepx_dxm1_bring_up` releases M1_RESET; PCIe link trains                   | logic analyser on M1_RESET + lspci |
| `dxrt_init()` succeeds                                                      | UART log                          |
| Reference YOLO inference on `dx_app` runs to completion                    | UART log + frame output           |
| `shut_down` cleanly drops PCIe link; rails are quiescent                    | scope on DEEPX rails              |

## Orchestrator design

The rig's controller runs a thin Python harness driving:

* `gdbserver` over SWD for both Renesas (post-mortem only) and GD32 (flash + reset).
* `screen` / `picocom` capturing the UART consoles to timestamped files.
* `sigrok-cli` for the logic-analyser captures.
* `iperf3`, `ping`, `dhclient` over the Ethernet partner.
* `hostapd_cli`, `wpa_cli` over the Wi-Fi partner.

Each test case is a function in `hil/tests/` that:

1. Resets the SoM to a known state.
2. Sends commands over `UART0` (or over `BRD_I2C` for bridge-only tests).
3. Captures the relevant signal trace (UART log, logic-analyser
   dump, iperf result).
4. Asserts expected vs actual.
5. Tags PASS/FAIL.

A run produces a single junit-XML file that CI can consume.

## Schedule (rough)

| Phase                              | Effort estimate        |
|------------------------------------|------------------------|
| Build the rig hardware             | 1-2 weeks bench time   |
| Wire UART + SWD + logic-analyser captures | a few days       |
| Implement power-tree tests         | 1 week                 |
| Implement GD32 bridge tests        | 1 week                 |
| Implement Ethernet tests            | 1 week                 |
| Implement Wi-Fi / BT / DEEPX tests  | 2 weeks                |
| CI integration                     | 1 week                 |

Total: ~2 months of one engineer's time once the rig hardware is
on the bench.  The bench hardware itself is ~$5-7k of off-the-shelf
parts plus the SoMs.

## Out-of-scope

* Multi-SoM fanout (target: after the single-board rig is proven).
* RF conducted measurements (handled by Murata/Infineon WHQL labs
  for FCC certification, not by the rig).
* Long-running burn-in tests (separate burn-in board with no
  fault-injection orchestration).

## See also

* [`docs/test-plan.md`](test-plan.md) -- the matrix the rig has to
  knock down green.
* [`docs/bring-up-v2n.md`](bring-up-v2n.md) -- manual bench bring-up
  that the rig automates.
