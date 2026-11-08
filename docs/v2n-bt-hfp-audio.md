# Bluetooth HFP/SCO audio (V2N / V2M)

Status: **pad routing confirmed, SSI channel assignment unconfirmed, device tree not written, bench-unverified.**
BT HFP/SCO audio is a product requirement (maintainer decision 2026-10-03).

## What is wired

The on-module Murata LBEE5HY2FY (CYW55513) exposes a PCM/I2S link to the SoC
(`metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`, `BT_I2S.*`; names are
in the module's frame, as with `BT_UART`):

| Net | SoC pad | Role (SSI, to confirm) |
|---|---|---|
| `BT_I2S.CLK` | P73 | bit clock, SSI7 SCK (to confirm) |
| `BT_I2S.WS` | P82 | frame sync, SSI7 WS (to confirm) |
| `BT_I2S.DI` | P83 | SSI7 SDATA (module input, SoC playback) (to confirm) |
| `BT_I2S.DO` | P81 | SSI8 SDATA (module output, SoC capture) (to confirm) |

The SSI7/SSI8 split (from private audit notes, not in this repo) is why a plain single-SSI card does not fit:
SCK/WS and one data line belong to SSI7, the other data line to SSI8, like
the TAS2563 pair (SSI1 pin master, SSI2 slave; patch 0014). Treat the
SSI7/SSI8 assignment as to-confirm against the pin-function table.

## Why no device tree yet

The pin function numbers for P73/P81/P82/P83 and the SSI7-pin-master /
SSI8-slave setup are not in this repo and no BSP kernel source was
reachable to read them. Pin functions are never guessed here (a wrong one
muxes a live pad), and `metadata/pinmux/v2n.yaml` carries no `func` field to
generate from. Not done, in order:

1. Read the function numbers for the four pads from the RZ/V2N hardware
   manual PFC table; add them to `metadata/pinmux/v2n.yaml` (the pin function numbers live there, not in a dtsi), then generate the `sound_bt_pins` group from it into the V2N-family SoM dtsi `e1m-v2n-som.dtsi` (the CYW55513 is on-module, so not `e1m-x-evk.dtsi`).
2. `&ssi7`/`&ssi8` under `&rcar_sound`, in `e1m-v2n-som.dtsi` (SSI8 as slave of SSI7's pins, the
   same mechanism as patch 0014), pad-side master/slave per step 3.
3. Which side drives SCK/WS (module PCM master vs SoC) and I2S vs PCM mode
   with the shipped firmware: unverified (Murata firmware dependent).
4. A card with the in-tree `linux,bt-sco` dummy codec
   (`CONFIG_SND_SOC_BT_SCO`) on a simple-audio-card or audio-graph-card2
   link, 8 kHz mono (narrowband) / 16 kHz (wideband).

## Userspace path (once the card exists)

BlueZ over the existing `hci0` (hci_uart, `wifi-bt.cfg`) with either
PipeWire/WirePlumber's native HFP backend (`bluez5` `hfp_hf`/`hsp_hs`) or
oFono + the BlueZ HFP profile, with SCO routed to the PCM link, not the
HCI transport: the controller needs its PCM routing set (vendor HCI
command / `.hcd`), to be confirmed with the Murata firmware.

## HIL spec (SCO loopback) - not run

1. Pair a headset with `bluetoothctl`; connect the HFP profile.
2. Confirm the SCO link is up (`btmon`: `Synchronous Connection Complete`).
3. Capture on the SoC (`arecord -D <bt-card> -f S16_LE -r 8000 -c 1`) while
   speaking into the headset; play back a tone with `aplay` and listen on the
   headset.
4. Expect clean audio both ways; record the clock master and rate used.
