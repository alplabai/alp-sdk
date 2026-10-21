# E1M-AEN801 on-board RP2040 debug probe

On the E1M EVK 2641-R3 the on-board RP2040 (`U55`) is the debugger for the
E1M-AEN801 (MRAM flash and debug over SWD) and the USB-to-UART console
bridge. A SEGGER J-Link is optional: it stays supported as the alternative
path (see [`debugging-aen.md`](debugging-aen.md) and
[`aen-bench-bringup.md`](aen-bench-bringup.md)).

> **Status: not bench-verified.** This page records the contract and the
> owner decisions of
> [#405](https://github.com/alplabai/alp-sdk/issues/405). The probe firmware
> has no EVK board config yet and its two-channel USB layout is not verified on
> RP2040 hardware (see the firmware repository README, "Status"). Nothing here
> has been run against an AEN module through the RP2040. Every J-Link flow in
> the AEN docs remains the only bench-proven path.

## Hardware (owner decision, 2026-10-19)

- `U55` (RP2040) flashes and debugs the E1M-AEN (MRAM) over SWD.
- Switch `S7` in the RP2040 position routes module `UART0` (`F2` / `G2`) to a
  USB CDC serial port on `J5`.

## Firmware

[`alplabai/rp2040-debugprobe-firmware`](https://github.com/alplabai/rp2040-debugprobe-firmware)
(public, MIT, derived from `raspberrypi/debugprobe` `debugprobe-v2.3.1`) owns
the firmware, its releases and its CI (ADR 0031). alp-sdk keeps only the
contract, board/runner representation and the HIL flow.

## USB identity and channel roles

One USB device, three functions. Host tooling must identify the serial ports
by **interface string**, not by enumeration order.

| Interface | Function | Interface string | UART |
|---:|---|---|---|
| 0 | CMSIS-DAP v2 (bulk) | `CMSIS-DAP v2 Interface` | n/a (SWD) |
| 1, 2 | CDC-ACM 0 | `Alp SE-UART` (secure-enclave ISP / recovery) | UART0 |
| 3, 4 | CDC-ACM 1 | `Alp App Console` (application console) | UART1 |

CDC1 is compile-time optional per board; without it the build is single-CDC
(interfaces 0-2, string `CDC-ACM UART Interface`). The product string contains
`CMSIS-DAP`, so OpenOCD, pyOCD and probe-rs detect the probe.

| Field | Value |
|---|---|
| USB VID:PID | **TBD** pending the dedicated-ID decision, coordinated with `alplabai/alp-studio#52`. The firmware currently uses the upstream `0x2E8A:0x000c` as a placeholder only. |

Which EVK UART feeds `UART0` / `UART1` on the 2641-R3 is TBD until the
firmware gains its EVK board config; the schematic is the source.

## Vendor commands (planned/implemented in the firmware repository)

I2C transfers, named-pin control and a power/energy stream are CMSIS-DAP v2
vendor commands (request IDs `0x80`..`0x9F`) on interface 0. The protocol is
specified in the firmware repository README, not duplicated here. The host
side for energy measurement is described in
[`measuring-inference-energy.md`](measuring-inference-energy.md).

## Flash and debug from the host

- **Documented here: nothing beyond detection.** Neither the issue nor the
  firmware README gives verified OpenOCD or pyOCD steps (target config, MRAM
  flash algorithm) for the AEN. They are TBD until bench-verified; do not
  infer them from the J-Link flows.
- **SDK tooling:** `west flash`/`west debug` for the AEN board use the J-Link
  and `alif_flash` (SETOOLS) runners today, and
  `scripts/flash_backends/swd_probe.py` covers only the GD32 supervisor. An
  RP2040 (CMSIS-DAP) flash backend and `tan` support are open work under #405.
