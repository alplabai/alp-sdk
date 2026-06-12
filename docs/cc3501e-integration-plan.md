# CC3501E Integration Plan (SWRU626 Deep-Dive)

**Date:** 2026-05-12
**Owner:** alpCaner
**Status:** Research-only. Informs the first production firmware drop in
`alplabai/cc3501e-firmware` and the v0.x follow-ups on the alp-sdk host side.
**Trigger:** Maintainer asked for a deep SWRU626 read before host + firmware
are wired the first time, so the wire protocol does not need a v1→v2 break
the moment real packets start flowing.
**Sources read:**

- TI SWRU626 Technical Reference Manual, December 2025 (~500 pages),
  ToC + chapters 2 (Architecture), 7 (Power/Reset/Clock), 10 (Boot/Bootloader),
  11 (Host DMA), 13 (GPT), 16 (IOMUX/GPIO), 17 (UART), 18 (SPI), 19 (I2C),
  21 (SDIO), 24 (ADC).
- TI CC3501E Datasheet **SWRS343**, December 2025 (CC350xE family,
  CC3501E SKU = 56-pin QFN 7 × 7 mm, 2.4 GHz Wi-Fi 6 + BLE 5.4).
- TI CC35xx Errata **SWRZ167** (referenced; not deep-read in this pass —
  noted as an open follow-up).
- alp-sdk repo sources listed under §4 below.

> **Acid-test ground rule (ADR 0005).** Anything in this plan that touches
> the TI SimpleLink CC33xx SDK headers, TI's BSD-3 + restricted-use code,
> or M33-side register programming **belongs in `alplabai/cc3501e-firmware`,
> NOT in this repo**. The host side stays Apache-2.0 clean and talks to
> the chip only through the wire protocol in
> [`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h).

---

## 1. Context

### 1.1 CC3501E in the E1M-AEN platform

The CC3501E is the wireless brain on every E1M-AEN module. The Alif Ensemble
application SoC has no integrated Wi-Fi/BLE radios for the AEN family, so
the CC3501E is treated as an on-module coprocessor with its own Cortex-M33,
its own external xSPI flash (4 MB on the E1M-AEN; the SKU table in SWRS343
also lists 8 MB and PSRAM-augmented variants), and its own Alp-authored
firmware.

The CC3501E also doubles as a GPIO + camera-LDO expander: per
[`metadata/e1m_modules/aen/from-cc3501e.tsv`](../metadata/e1m_modules/aen/from-cc3501e.tsv)
and [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv)
the E1M pads `IO11 / IO13 / IO15..IO21` plus `CAM_EN_LDO0/1` land on CC3501E
GPIOs, so the CC3501E firmware proxies those signals on behalf of the Alif.

### 1.2 Inter-chip transport (SPI1)

The Alif is master and the CC3501E is peripheral. SPI1 maps:

| Signal     | Alif pad   | CC3501E pad      | SWRU626 reference |
|------------|------------|------------------|-------------------|
| SPI1.SCLK  | `P14_6`    | `GPIO_27`        | §18.2, §16.6      |
| SPI1.MOSI  | `P14_5`    | `GPIO_28`        | §18.2, §16.6      |
| SPI1.MISO  | `P14_4`    | `GPIO_29`        | §18.2, §16.6      |
| `WIFI_EN`  | `P15_5`    | (power enable)   | datasheet pin 49 ctx |
| `E_WIFI.NRST` | `P15_1_FLEX` | pin 49 `nRESET`  | SWRU626 §7.2, §7.1.5 |

The wire-protocol contract is already frozen at v1:
[`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h)
(`ALP_CC3501E_PROTOCOL_VERSION == 1`, 4-byte header, payload ≤ 512 B).
This document does NOT propose a v2 break — it proposes additive
flag/opcode extensions that v1 reserved.

### 1.3 ADR 0005 split (host vs firmware) — what stays where

| Responsibility                                          | Lives in                          |
|---------------------------------------------------------|-----------------------------------|
| Wire-protocol header (commands, frames, flags, events)  | **alp-sdk** `<alp/protocol/cc3501e.h>` |
| Alif-side SPI client (host driver)                      | **alp-sdk** `chips/cc3501e/cc3501e.c` |
| `<alp/iot.h>` / `<alp/ble.h>` route-through             | **alp-sdk** `src/iot_zephyr.c`, `ble_zephyr.c` (TBD) |
| Bridge architecture doc, GPIO contract                  | **alp-sdk** `docs/cc3501e-bridge.md` |
| Flasher CLI shape                                       | **alp-sdk** `firmware/cc3501e/flash.py` |
| Prebuilt signed firmware binaries                       | **alp-sdk** `firmware/cc3501e/prebuilt/` |
| **All M33-side register programming (SPI, SDIO, DMA, GPIO IOMUX, GPT, ADC, UART, I2C, power, clock, IRQ, BOR, WDT)** | **cc3501e-firmware** |
| TI SimpleLink CC33xx SDK + BLE host                     | **cc3501e-firmware** (vendored submodule) |
| MCUboot vendor-image signing + initial-programming tooling | **cc3501e-firmware** |
| TI BL1/BL2 chain, OTP root-of-trust                     | **cc3501e-firmware** (governs build artefacts) |

> **Net for alp-sdk:** the host driver never `#includes` TI SimpleLink
> headers, never references CC3501E peripheral registers by name, and
> never assumes anything about M33-side memory layout. It only sees the
> 4-byte protocol header + payload.

### 1.4 Existing alp-sdk surfaces (what we are extending, not rewriting)

- [`chips/cc3501e/cc3501e.c`](../chips/cc3501e/cc3501e.c) — 133-LOC
  v0.3 host stub. Implements `cc3501e_init`/`reset`/`request`/
  `get_version`/`set_event_callback`/`deinit` against a generic
  `alp_spi_t` bus. Reset returns `ALP_ERR_NOSUPPORT` when no GPIO pins
  are bound; otherwise drives `enable_pin` + `reset_pin` low → high.
  Synchronous request is single-frame full-duplex with no busy/ready
  GPIO yet.
- [`include/alp/chips/cc3501e.h`](../include/alp/chips/cc3501e.h) —
  public API surface. Declares the lifecycle, the request shape, the
  event callback type, and the `struct cc3501e` (scratch buffers
  inline, sized to `ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD`).
- [`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h)
  — the v1 wire protocol. Opcode groups: 0x00-0x0F meta, 0x10-0x2F
  Wi-Fi + sockets, 0x30-0x4F BLE, 0x50-0x5F GPIO proxy, 0x60-0x6F
  power/camera, 0x70-0x7F diagnostics, 0x80-0xFF reserved.
- [`docs/cc3501e-bridge.md`](cc3501e-bridge.md) — bridge architecture,
  boot model, firmware-side GPIO contract.
- [`firmware/cc3501e/README.md`](../firmware/cc3501e/README.md) —
  firmware-side bootstrap checklist + ADR-0005 rationale.
- [`firmware/cc3501e/protocol-version.txt`](../firmware/cc3501e/protocol-version.txt)
  — currently `1`. Must be bumped in lockstep with
  `ALP_CC3501E_PROTOCOL_VERSION` if any of the changes in §5 require a
  break.
- [`firmware/cc3501e/flash.py`](../firmware/cc3501e/flash.py) — flasher
  stub (CLI shape locked; real impl when first binary lands).
- [`tests/zephyr/chips/src/main.c`](../tests/zephyr/chips/src/main.c)
  — Zephyr Ztest harness with `test_cc3501e_init_null_args` +
  `test_cc3501e_calls_reject_uninitialised`. Currently 2 tests; should
  grow with every new opcode batch.

---

## 2. Audit method

### 2.1 SWRU626 sections read (with page ranges)

| Chapter                                          | Pages       | Used for §3 sub-section |
|--------------------------------------------------|-------------|--------------------------|
| 1 Read This First                                | 11–12       | Reading conventions      |
| 2 Architecture Overview                          | 13–22       | §3 framing               |
| 7 Power, Reset, Clock Management                 | 846–891     | §3.9                     |
| 10 Device Boot and Bootloader                    | 1040–1047   | §3.11                    |
| 11 Direct Memory Access (DMA)                    | 1048–1161   | §3.3                     |
| 13 General Purpose Timers (GPT)                  | 1163–1231   | §3.7                     |
| 16 General Purpose Input/Output (GPIOs / IOMUX)  | 1292–1546   | §3.4                     |
| 17 UART                                          | 1547–1590   | §3.5                     |
| 18 Serial Peripheral Interface (SPI)             | 1591–1635   | §3.1                     |
| 19 Inter-Integrated Circuit (I2C)                | 1636–1692   | §3.8                     |
| 21 Secure Digital Input/Output (SDIO)            | 1774–1828   | §3.2                     |
| 24 Analog-to-Digital Converter (ADC)             | 1934–2032   | §3.6                     |

### 2.2 SWRU626 sections only skimmed (open follow-ups)

These chapters were not load-bearing for the host-side plan and were
not deep-read. None of them affects the wire-protocol design today,
but each is a known gap if/when the corresponding peripheral becomes
a proxied surface:

- §3 Arm Cortex-M33 Processor (M33 internals — firmware-side concern).
- §4 Memory Map (M33 internals).
- §5 Interrupts and Events (read enough to confirm shared peripherals
  mux + GPIO interrupt path; full event-manager table not transcribed).
- §6 Debug Subsystem (DEBUGSS) — relevant only for the flash.py path.
- §8 Memory Subsystem (MEMSS) including xSPI / OTFDE — affects firmware
  build artefacts, not the host.
- §9 Hardware Security Module — referenced via §10 Chain of Trust; the
  full register map is firmware-side.
- §12 OTP — same.
- §14 SysTimer, §15 RTC — firmware-side concerns.
- §20 SDMMC, §22 I2S, §23 PDM — present on the SKU per SWRS343 but not
  proxied through the wire protocol today.
- §25 CAN 2.0 — present on the SKU per SWRS343; firmware-only today.

### 2.3 SWRU626 sections that were searched for but DO NOT EXIST

> **Important caveat the maintainer asked us to surface.** The SWRU626
> TRM does **not** contain a "SimpleLink driver-host wire interface"
> chapter. The TRM is the chip TRM: it documents the on-die peripherals
> the M33 sees, including SPI peripheral mode (§18, M33 as SPI peripheral)
> and SDIO peripheral mode (§21, M33 as SDIO peripheral) — those are the
> two physical bus options the external host (Alif) can talk to.
>
> The **wire-format protocol** the external host actually speaks (the
> NetCP / NWP / SimpleLink command messages, the "device" + "command"
> + "response" framing TI's `sl_*` driver wraps) lives in TI's
> **SimpleLink CC33xx SDK** documentation, not in SWRU626. We
> intentionally do **not** read that spec here — per ADR 0005 the alp-sdk
> host side does NOT speak that protocol. The CC3501E firmware does:
> the firmware runs TI's `sl_*` driver internally, and exposes a small
> Alp-authored protocol (the `<alp/protocol/cc3501e.h>` opcodes) to the
> Alif side.
>
> §3.10 below documents the implications.

### 2.4 alp-sdk surfaces inspected

Files read in full or in relevant excerpt:

- `chips/cc3501e/cc3501e.c` (133 LOC, full read).
- `include/alp/chips/cc3501e.h` (114 LOC, full read).
- `include/alp/protocol/cc3501e.h` (235 LOC, full read).
- `docs/cc3501e-bridge.md` (258 LOC, full read).
- `docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md` (full read).
- `docs/soms/aen.md` (full read).
- `metadata/e1m_modules/aen/inter-chip.tsv` (full read).
- `metadata/e1m_modules/aen/from-cc3501e.tsv` (full read).
- `firmware/cc3501e/README.md` (full read).
- `firmware/cc3501e/flash.py` (header excerpt — confirms CLI contract).
- `firmware/cc3501e/protocol-version.txt` (1 line).
- `tests/zephyr/chips/src/main.c` (CC3501E test block).
- `docs/gd32-bridge.md` (excerpt — used as a reference structure for
  the firmware-tree side of a sibling coprocessor).

---

## 3. Peripheral-by-peripheral plan

Each subsection is structured the same way:

1. **Register-level summary** — which CC35xx registers carry the load,
   named per SWRU626 conventions. Host side never touches these directly.
2. **Host-side knobs** — what the Alif-side `alp_spi_t` config or
   `alp_gpio_t` config needs to expose.
3. **DMA descriptor format** — what the M33 firmware programs on its
   side for the peripheral. Listed for traceability only.
4. **Transfer cadence / IRQ semantics** — what the wire protocol has
   to express, and at what latency.
5. **ADR-0005 split** — explicit host vs firmware responsibilities.
6. **Wire-protocol additions** — only the deltas to
   `<alp/protocol/cc3501e.h>`. Opcode numbering respects the existing
   group ranges (0x80–0xFF still reserved).

### 3.1 SPI (SWRU626 §18, pp. 1591–1635)

This is the bus between the Alif and the CC3501E. There are two
distinct SPI surfaces involved:

- **Alif's SPI1 in controller mode** — owned by alp-sdk. The Alif's
  Ensemble HAL (vendor side) configures its own SPI1 controller. The
  host driver consumes it via `alp_spi_t`.
- **CC3501E's SPI in peripheral mode** — owned by cc3501e-firmware.
  SWRU626 §18 documents what the firmware has to set up so the M33
  can RX/TX the frames the Alif master is clocking out.

#### 3.1.1 Register-level summary (firmware side, for traceability)

Per SWRU626 §18.6 the firmware programs (selection, not full set):

- `SPI.CTL0` — protocol format (`FRF`), data size (`DSS=8`), MSB-first,
  parity, chip-select control (`HWCSN`, `CSCLR`).
- `SPI.CTL1` — master/slave bit `MS`, peripheral-output enable `POD`,
  receive timeout `RTOUT` (peripheral mode only — see §3.1.4),
  repeat-TX `REPTX`.
- `SPI.CLKCFG0`/`CLKCFG1` — `PRESC` + `SCR` divider, `DSAMPLE` for
  delayed sampling.
- `SPI.IFLS` — FIFO trigger levels (`TXSEL` / `RXSEL`).
- `SPI.DMACR` — RX/TX DMA enable.
- `SPI.IMASK` / `RIS` / `MIS` / `ISET` / `ICLR` / `IMSET` / `IMCLR` —
  interrupt mask + status families.
- `SPI.TXDATA` / `RXDATA` / `STA` / `TXFHDR*` / `RXCRC` / `TXCRC`.

FIFOs are 32 × 8-bit when `DSS ≤ 8` or 16 × 16-bit when `DSS > 8`
(SWRU626 §18.1.1). The wire protocol uses `DSS = 8` so we get the
deeper 32-byte FIFOs.

#### 3.1.2 Host-side knobs

The host driver opens SPI1 via:

```c
alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
    .bus_id        = E1M_SPI1,
    .freq_hz       = 8000000,          // current default; TBD by signal-integrity
    .mode          = ALP_SPI_MODE_0,   // CPOL = 0, CPHA = 0
    .bits_per_word = 8,
});
```

Already exposed in `<alp/chips/cc3501e.h>`. **No new host-side knob is
required at v0.3.** When v0.3.x lands the busy/ready GPIO (see §3.4.3
below), a single optional `alp_gpio_t *busy_pin` field on
`struct cc3501e` suffices — no new public API.

#### 3.1.3 DMA descriptor format (firmware side, for traceability)

Per SWRU626 §11.3.1 the SPI peripheral surfaces 4 DMA channels at
peripheral indices 4 (SPI0 RX), 5 (SPI0 TX), 6 (SPI1 RX), 7 (SPI1 TX).
Each channel uses fixed→incremental (or vice versa) addressing
(§11.3.3 mode 2/3), block size matched to the configured FIFO trigger
level. The firmware will own the channel-priority decision (two
channels can be high-priority per §11.3.8) and will most likely make
the SPI1 RX channel high-priority so incoming frames clock out the
FIFO before it overflows (`RXOVF` per §18.3.3) at sustained 8 MHz.

#### 3.1.4 Transfer cadence / IRQ semantics

- **Today's host model (v0.3):** synchronous full-duplex. The Alif
  master clocks header (4 B) + payload (≤ 512 B) in a single
  `alp_spi_transceive`. The MISO line is assumed to carry the
  firmware's reply within the same transaction.
- **Limitation (called out in `cc3501e.c` comments):** "the firmware
  needs to be running for them to mean anything" — there is no
  busy/ready signal, so on a real chip the firmware may not have
  buffered a reply yet when the Alif starts clocking. The host can
  observe `0x00` MISO bytes and decide nothing is there.
- **v0.3.x fix path (already foreseen in the source comments):**
  add a separate "MISO-not-busy" GPIO from the CC3501E to the Alif.
  Two options surveyed against SWRU626:
  - **Option A — software-driven GPIO.** Firmware drives any free
    CC3501E GPIO low while it is preparing the reply, releases it
    when the TX FIFO has the response queued. Host polls via
    `alp_gpio_read` (preferred for v0.3.x).
  - **Option B — SPI peripheral receive-timeout (`SPI.CTL1[29:24]
    RTOUT`).** Per SWRU626 §18.3.3, the SPI peripheral can flag a
    receive-timeout IRQ when SCK is idle for N SOC_CLK cycles. This is
    a firmware-side liveness watchdog, NOT a host-side mechanism — but
    it lets the firmware drop a stale frame cleanly. Worth wiring on
    the firmware side as defence in depth.
- **Async event delivery — already accounted for in the protocol.**
  Flag bit 1 (`ALP_CC3501E_FLAG_ASYNC_EVENT`) distinguishes solicited
  replies from unsolicited notifications (Wi-Fi disconnect, BLE adv
  report, GPIO interrupt). The host's RX thread (planned, not in v0.3)
  will demux on this bit.

#### 3.1.5 ADR-0005 split

| Concern                                            | Side       |
|----------------------------------------------------|------------|
| Alif's `SPI1` controller-mode config (master)      | host (alp-sdk via Alif HAL) |
| `SPI.CTL0/CTL1/CLKCFG*/IFLS/DMACR` programming     | firmware   |
| FIFO trigger level tuning                          | firmware   |
| `RXOVF` / `RTOUT` ISRs                             | firmware   |
| Busy/ready GPIO (raise / lower)                    | firmware   |
| Busy/ready GPIO (poll / read)                      | host       |
| Reply demux on `FLAG_ASYNC_EVENT`                  | host       |

#### 3.1.6 Wire-protocol additions for SPI

- **Reserved flag bit 2 (`ALP_CC3501E_FLAG_FRAME_CONTINUATION`).**
  Already noted in `<alp/protocol/cc3501e.h>` as "reserved in v1;
  v2 will land alongside the BLE long-write path". The SPI peripheral
  has no inherent transaction-size limit beyond what FIFOs + DMA
  block size enforce, so the constraint is purely the 512-byte
  `ALP_CC3501E_MAX_PAYLOAD`. **Decision:** keep it reserved for v1,
  promote when BLE long-write or socket > 512 B is implemented.
- **No new opcode for SPI itself.** SPI is the transport, not a
  proxied peripheral. No `CMD_SPI_*` is needed.

### 3.2 SDIO (SWRU626 §21, pp. 1774–1828)

SDIO is the **alternative** host-CC3501E bus per
`docs/cc3501e-bridge.md` — wired in hardware on the E1M-AEN module
but multiplexed with the SD-card slot via board-level resistors
(only one of {SDIO-to-CC3501E, SD-card} can be active at a time).
The current v0.3 driver only uses SPI1. SDIO is documented here for
the future where the wire protocol's bandwidth ceiling becomes a
limiter (single-frame UDP/TCP-payload offload at SPI 8 MHz tops
out around 1 MB/s ignoring overhead).

#### 3.2.1 Register-level summary (firmware side)

Per SWRU626 §21:

- `SDIO_CORE` registers (`CCCR00..C4`, `FBR1R*`, `CIS*ADDR`) — the
  SDIO 2.0 standard CCCR / FBR / CIS area, mapped at byte offsets
  for external-host CMD52/CMD53 access (§21.3.3, Table 21-1).
- `SDIO_CARD_FN1` registers (`DATAFIFO`, `RXTHR`, `TXIRQTHR`,
  `DMABLKTHR`, `FLUSHCMD`, `RXBBUF`, `IRQSTA`, `IRQMASK`, `IRQCLR`).
- IRQ-ACK pair `HCIACK` / `HCINACK` / `HCIWRRET` (§21.3.6.1, Table 21-2).

Function #1 is dedicated to data transfer (128-byte TX FIFO,
256-byte RX FIFO; SDIO block size up to 128 B).

#### 3.2.2 Host-side knobs

If/when SDIO is exposed, the host driver opens via:

```c
alp_sdio_t *bus = alp_sdio_open(&(alp_sdio_config_t){
    .bus_id    = E1M_SDIO,
    .bus_width = 4,                       // 1 or 4-bit; TBD by signal-integrity
    .freq_hz   = 50000000,                // SDIO 2.0 50 MHz default
    .obi       = ALP_SDIO_OBI_DEDICATED,  // out-of-band IRQ on GPIO_29 per datasheet (TBD)
});
```

Today `alp_sdio_*` does not exist as a public surface. **Adding it is
out of scope for the first v0.x SPI integration**; ship SPI first.

#### 3.2.3 DMA descriptor format (firmware side)

Per SWRU626 §11.3.1 indices 14/15 (SDIO RX/TX). Block size matches
`SDIO_CARD_FN1:DMABLKTHR[2:0]RXDMABLK` per §21.3.5.1, with the FIFO
threshold `RXTHR[7:2]VAL` set to trigger DMA before overflow.

#### 3.2.4 Transfer cadence / IRQ semantics

- **External host → CC3501E (RX path, §21.3.7.1):** host issues CMD53,
  data lands in 256-byte RX FIFO, firmware DMAs to SRAM. CRC-status
  on data bit 0 per SDIO spec.
- **CC3501E → external host (TX path, §21.3.7.2):** firmware loads TX
  FIFO, configures `TXIRQTHR` byte threshold, hardware asserts IRQ to
  the external host (in-band on data bus or out-of-band on a dedicated
  pin per §21.3.1). Host clears the IRQ via CMD52 write to
  `SDIO_CARD_FN1.ICLR`, then issues CMD53 to read.
- **Latency:** SDIO at 50 MHz 4-bit gives a theoretical ~25 MB/s on
  block transfers. The protocol's 512-byte payload cap becomes the
  bottleneck, not the bus.

#### 3.2.5 ADR-0005 split

| Concern                                | Side       |
|----------------------------------------|------------|
| Alif's SDIO-host controller config     | host       |
| CC3501E `SDIO_CORE` + `SDIO_CARD_FN1` programming | firmware |
| FIFO thresholds, DMA block sizes       | firmware   |
| Out-of-band IRQ pin assignment         | TBD (signal-integrity / pin allocator) |
| CMD52 / CMD53 driving                  | host       |

#### 3.2.6 Wire-protocol additions for SDIO

- **None required at v1.** SDIO is a transport. The same opcodes carry
  over the same frame format. **Decision point** if SDIO ships before
  v2: do we let the protocol payload size grow beyond 512 B on SDIO
  to amortise the per-frame overhead? Probably yes via the existing
  `FLAG_FRAME_CONTINUATION` bit — same plan as the BLE long-write
  path.

### 3.3 DMA (SWRU626 §11, pp. 1048–1161)

Pure firmware-side. The host driver doesn't see it.

#### 3.3.1 Register-level summary (firmware side)

`HOST_DMA` registers: `CHCTL0/CHCTL1`, `PRIOCFG`, and per-channel
`CHnTIPTR` / `CHnOPTR` / `CHnTCTL` / `CHnJCTL` / `CHnSTA` (offsets
0x1000, 0x2000, 0x3000, … per §11.4 register map).

#### 3.3.2 Capability snapshot

- 14 independent channels.
- Up to 2 channels can be promoted to high-priority; remaining
  channels arbitrate round-robin (§11.3.8).
- Job size up to 16 384 B. Block size 1–63 words, words of 8/16/32 bit
  (§11.3.4). Supports unaligned start address + unaligned job size on
  32-bit words (§11.3.5).
- Peripheral request indices relevant to host comms: 4/5 (SPI0 RX/TX),
  6/7 (SPI1 RX/TX), 14/15 (SDIO RX/TX), 18 (ADC), 20 (PDM), 22/23
  (Wi-Fi/BLE Host Interface — INTERNAL to the chip; this is the M33 ↔
  Wi-Fi/BLE radio core, NOT the external-host link).

#### 3.3.3 ADR-0005 split

100% firmware-side. The host driver MUST NOT mention DMA channels.

#### 3.3.4 Wire-protocol additions for DMA

**None.** DMA is invisible to the host.

### 3.4 GPIO / IOMUX (SWRU626 §16, pp. 1292–1546)

This is the protocol-relevant peripheral after SPI. The host's
`ALP_CC3501E_CMD_GPIO_CONFIGURE` / `_WRITE` / `_READ` /
`_SET_INTERRUPT` opcodes drive it.

#### 3.4.1 Register-level summary (firmware side)

Per SWRU626 §16:

- `IOMUX.GPIOnPCFG` — function select per pin. `0x2` is "base
  function — GPIO" per §16.3.1.
- `IOMUX.GPIOnCFG` — input enable (`IE`), output disable (`OUTDIS`),
  drive strength (`IOSTR`), open-drain override (`ODISOVEN`).
- `IOMUX.GPIOnCTL` — output value (`OUT`), output-override enable
  (`OUTOVREN`).
- `IOMUX.GPIOnPCTL` — pull-up / pull-down / no-pull / IP-pull (§16.6).
- `IOMUX.GPIOnECTL` — edge-detect config (rising / falling / level /
  none), event-clear (`CLR`).

#### 3.4.2 Open-drain output (firmware contract)

`docs/cc3501e-bridge.md` already calls out the M.2 `W_DISABLE1/2`
contract. Per §16.6 the firmware implements this via the `ODISOVEN`
override:

- On configure with direction `ALP_CC3501E_GPIO_DIR_OPEN_DRAIN`:
  - Set `IE = 1` (read-back), `OUTDIS = 0`, `ODISOVEN = 1`, `OUTOVREN = 1`.
- On write 0: set `OUT = 0` → output buffer drives low.
- On write 1: set `OUT = 1` AND set `OUTDIS = 1` simultaneously →
  pad goes Hi-Z. The M.2 card's pull-up handles the rail.

> **Open question (§7.2 below):** the protocol currently passes the
> direction as `uint8_t direction` with `0 = input, 1 = output`. We
> need to define `2 = open-drain` to make this explicit, and possibly
> `3 = open-drain-with-internal-pullup` for completeness. Today no
> ALP_CC3501E_GPIO_DIR_* enum exists in the header — host firmware
> just sends magic numbers.

#### 3.4.3 Busy/ready GPIO for SPI handshake

Cross-reference §3.1.4 Option A. Pick any free CC3501E GPIO for the
"firmware-ready-to-reply" signal; route it back to the Alif via the
E1M's `from-cc3501e.tsv` mapping. **Pin assignment: TBD** — must come
out of the pin allocator + signal-integrity review, not invented here.

#### 3.4.4 Edge-triggered IRQ forwarding (firmware contract)

Already specified in `docs/cc3501e-bridge.md` for `M2E_UART_WAKE`,
`M2E_SDIO_WAKE`, `BMI323_INT1`. Per §16.4 the firmware:

1. Configures `IOMUX.GPIOnECTL[1:0] EVTDETCFG` to publish to the AON
   event manager.
2. Receives the IRQ via AON event manager → NVIC.
3. Emits an `ALP_CC3501E_EVT_GPIO_INTERRUPT` (opcode `0x54`) frame
   over SPI to the Alif (already in v1).

> **Important per §16.4 note:** when configuring edge detection, the
> recommended sequence is "configure all bits EXCEPT `EVTDETCFG` + `IE`
> first, then write `IE`, then write `EVTDETCFG`". Firmware must
> follow this or spurious edges will fire during configure. The host
> doesn't see this; it's a firmware contract item.

#### 3.4.5 ADR-0005 split

| Concern                                          | Side       |
|--------------------------------------------------|------------|
| `IOMUX.GPIOn*` programming                       | firmware   |
| Edge-detect configure-order workaround           | firmware   |
| Open-drain implementation                        | firmware   |
| Wire-protocol command/event encoding             | both (header) |
| Pin allocation (which CC3501E GPIO maps to which E1M pad) | metadata (alp-sdk) + cc3501e-firmware glue table |

#### 3.4.6 Wire-protocol additions for GPIO

- **Add direction enum to `<alp/protocol/cc3501e.h>`** to replace the
  current magic-number `uint8_t direction` field:

  ```c
  typedef enum {
      ALP_CC3501E_GPIO_DIR_INPUT        = 0,
      ALP_CC3501E_GPIO_DIR_OUTPUT       = 1,
      ALP_CC3501E_GPIO_DIR_OPEN_DRAIN   = 2,
      ALP_CC3501E_GPIO_DIR_OPEN_SOURCE  = 3, // reserved / probably unused
  } alp_cc3501e_gpio_dir_t;
  ```

  Wire-compatible with v1 (still `uint8_t`, values 0–1 unchanged).
  Adds explicit names for what cc3501e-firmware already accepts.

- **Optional: add `pull` enum** for the same reason — current header
  uses `0 = none, 1 = up, 2 = down`. Make it a named enum.

- **`ALP_CC3501E_CMD_GPIO_SET_INTERRUPT` payload format.** Today the
  protocol header declares the opcode but does NOT have a documented
  payload struct (cf. `alp_cc3501e_gpio_configure_t` is documented but
  the interrupt-config struct is missing). Add a struct:

  ```c
  typedef struct {
      uint8_t cc3501e_gpio;
      uint8_t trigger;  /* 0 = none, 1 = rising, 2 = falling, 3 = both, 4 = level-high, 5 = level-low */
      uint8_t debounce_us; /* 0 = no filter; firmware-side glitch-filter target */
      uint8_t reserved;
  } alp_cc3501e_gpio_set_interrupt_t;
  ```

  Add a matching event payload:

  ```c
  typedef struct {
      uint8_t cc3501e_gpio;
      uint8_t edge;     /* same enum as trigger */
      uint16_t reserved;
      uint64_t timestamp_us; /* firmware monotonic clock; LE */
  } alp_cc3501e_gpio_interrupt_evt_t;
  ```

  The `timestamp_us` is what lets the host correlate motion events
  from BMI323 with sensor reads (the 5 ms latency budget mentioned in
  `docs/cc3501e-bridge.md` is then verifiable at the host).

### 3.5 UART / USCI (SWRU626 §17, pp. 1547–1590)

The CC3501E has 3 UARTs (per SWRS343 §1 / SWRU626 §2.13.1). Per the
EVK schematic + `docs/cc3501e-bridge.md` § "Edge-triggered interrupt
forwarding", `M2E_UART_WAKE` is a wake-from-UART signal **forwarded
to the Alif as a GPIO event**, not a UART-payload pass-through. The
CC3501E does not currently expose any of its 3 UARTs as a proxied
peripheral to the Alif.

#### 3.5.1 Feature snapshot (firmware side)

Per §17.1: up to 4 Mbps, 8 × 8 TX/RX FIFOs, programmable trigger
levels (¼, ½, ⅞), 5–8 data bits, even/odd/stick parity, 1 or 2 stop
bits, hardware flow control, IrDA encoder, **LIN protocol support**
(LIN break/synch transmission and reception, automatic baud-rate
detection per §17.4). Host DMA via indices 0/1 (UART0), 2/3 (UART1),
24/25 (UART2) per §11.3.1.

#### 3.5.2 ADR-0005 split

100% firmware-side at v0.x. The protocol does not expose UART.

#### 3.5.3 Wire-protocol additions for UART

**None today.** When/if UART pass-through becomes a feature (e.g. for
a debug console proxied through the Alif), reserve a new opcode group
within `0x80..0xFF`:

```text
0xA0..0xAF  reserved — UART proxy
```

For now: don't allocate, don't reserve a struct.

### 3.6 ADC (SWRU626 §24, pp. 1934–2032)

CC3501E has an 8-channel, 12-bit SAR ADC per SWRS343 §1. SWRU626
§24.1 confirms 12 channels total (4 internal — temperature, supply,
two more — and 8 external on GPIOs), with internal 1.4 V reference
giving 1 Msps or external 1.8 V reference (via VPP) giving 2 Msps.
Window comparator, FIFO mode, two sample-time compare registers, and
DMA via peripheral index 18 per §11.3.1.

#### 3.6.1 ADR-0005 split

The ADC is *internal to the CC3501E*. Whether it should be proxied to
the Alif depends on whether E1M-AEN routes any analog signal to a
CC3501E ADC pin (it does not today — `from-cc3501e.tsv` shows GPIO_31,
GPIO_32, GPIO_33, GPIO_34, GPIO_15 routed as digital SPI1.CSn /
SPI1.SCK / SPI1.MOSI / SPI1.MISO secondary mux). **Net: not exposed
via wire protocol at v0.x.** If someday a board variant routes an
analog signal to a CC3501E ADC channel, add an opcode under
`0x80..0xFF` (proposed `0xB0..0xBF` reserved — ADC proxy).

#### 3.6.2 Wire-protocol additions for ADC

**None today.**

### 3.7 Timers / PWM — GPT (SWRU626 §13, pp. 1163–1231)

8 General-Purpose Timers / PWM channels per SWRS343 §1. SWRU626 §13.1
confirms 8 channels organised as 2 timer instances × 4 channels each,
32-bit counter, 8-bit prescaler, quadrature decoding, complementary
PWM with programmable dead-band, Park-Mode on fault, IR signal
generation, DMA request output, ADC trigger output. Synchronisable
across timers via `STARTCFG` (§13.3.9).

#### 3.7.1 ADR-0005 split

The CC3501E's PWM outputs aren't routed anywhere external on the
E1M-AEN today (`from-cc3501e.tsv` shows only SPI1 secondary mux on the
relevant pads). The Alif has its own timers + PWM via `<alp/pwm.h>`,
which the gd32-bridge and the Alif HAL already cover. **Net: don't
proxy.**

#### 3.7.2 Wire-protocol additions for timers

**None today.** Reserve `0xC0..0xCF` for a future "timer / capture
proxy" if it becomes a feature; do not allocate now.

### 3.8 I2C (SWRU626 §19, pp. 1636–1692)

2 × I2C per SWRS343 §1. SWRU626 §19.1 confirms 7- and 10-bit
addressing, controller + target modes, FIFO (8 × 8) per direction,
standard/fast/fast-mode-plus speeds (100 kbps / 400 kbps / 1 Mbps),
multi-target address capability, digital glitch filter, multi-master
support with arbitration, DMA via §11.3.1 indices 8/9 (I2C0) and
10/11 (I2C1).

#### 3.8.1 ADR-0005 split

Same logic as UART/ADC/PWM: the Alif has its own I2C surface via
`<alp/i2c.h>` and the E1M-AEN module's I2C devices (OPTIGA Trust M,
RV-3028-C7, TMP112, EEPROM N24S128 per `docs/soms/aen.md`) hang off
the Alif's LPI2C, NOT the CC3501E's. **Net: don't proxy.**

#### 3.8.2 Wire-protocol additions for I2C

**None today.**

### 3.9 Power management & reset (SWRU626 §7, pp. 846–891)

This is the most under-specified surface in the existing host stub
and it deserves the most attention before v0.4.

#### 3.9.1 Reset sources (SWRU626 §7.2.1, Table 7-4)

| Reset cause                       | Reset level         | Affects host? |
|-----------------------------------|---------------------|---------------|
| RSTN pin OR POR                   | Power-On Reset      | Yes — `cc3501e_reset()` |
| RVML / RVMH (rail voltage monitor, ~1.3 V)| Power-On Reset | Yes — uncatchable from host |
| Brownout detection (VDDMAIN < 1.71 V)| Device AON Reset | Yes — uncatchable, but firmware can report on next PING |
| M33 watchdog                      | Device AON Reset    | Indirectly — firmware should report reset cause on first PING reply |
| M33 self-reset (`CMD_RESET`)      | Device AON Reset    | Yes — already in v1 protocol as opcode `0x02` |
| Debug-subsystem reset             | Debug-aware path    | Yes — flash.py / dev path |

The brownout threshold is **VDDMAIN < 1.71 V** (SWRU626 §7.1.4) — the
1.3 V number cited in `docs/cc3501e-bridge.md` Table is from the RVM
comparators, which trip first. Both reset the device cold.

#### 3.9.2 Boot timing (SWRU626 §7.1.5, Table 7-3)

Already documented in `docs/cc3501e-bridge.md` (Table — T1+T2+T3+T4 ≈
900 ms). The host's bring-up code must hold off `ALP_CC3501E_CMD_PING`
for at least 1 s after nRESET release. The current `cc3501e_reset()`
in `cc3501e.c` notes this is "v0.3.x once `alp_delay_*` lands". That
delay helper is still missing — it's a clear v0.3.x blocker.

> **Important SWRU626 §7.1.5 power-up note** that the existing
> `cc3501e_reset()` doesn't honour: "The nReset pin should be held low
> for at least 10 µs after stabilization of the external power
> supplies." The current host code raises both `enable_pin` and
> `reset_pin` back-to-back with no delay in between. On a real chip
> the firmware will probably still boot — the supplies have been
> stable for milliseconds before this point — but the 10 µs hold is
> the spec'd minimum and we should respect it.

#### 3.9.3 Power states (SWRU626 §7.1.2, Table 7-1)

The CC35xx has 4 power states: Shutdown / Sleep / Idle / Active.
Within Active, the M33 + Wireless subsystem can each independently be
Active / Idle / Sleep / OFF — 9 combinations per Table 7-1.

For host-controlled power management, the only externally-visible
transitions are:

- **Active → Shutdown** — via `WIFI_EN = low`. Drives the device into
  reset; no firmware running.
- **Active → Sleep / Idle** — driven internally by the firmware's
  TI SimpleLink Power Manager. The host can request a power policy
  (e.g. "low-power for the next 60 s" or "stay in low-latency mode")
  via a new opcode, but the host should never directly control the
  M33's sleep state.

#### 3.9.4 ADR-0005 split

| Concern                                          | Side       |
|--------------------------------------------------|------------|
| Drive `WIFI_EN` / `nRESET` GPIOs                 | host (via `alp_gpio_*`) |
| Honour 10 µs reset-hold + 900 ms boot budget     | host (needs `alp_delay_*`) |
| Reset-cause reporting on first PING              | firmware (must respect existing PING opcode) |
| TI SimpleLink Power Manager policy               | firmware |
| Brownout / WDT recovery + report                 | firmware |

#### 3.9.5 Wire-protocol additions for power management

- **Extend `ALP_CC3501E_CMD_PING` reply** to optionally carry
  reset-cause metadata. Today the protocol header doesn't define the
  PING reply payload at all. Propose:

  ```c
  typedef struct {
      uint8_t  protocol_version;   /* = ALP_CC3501E_PROTOCOL_VERSION */
      uint8_t  reset_cause;        /* see alp_cc3501e_reset_cause_t */
      uint16_t firmware_version;   /* same as CMD_GET_VERSION reply */
      uint64_t uptime_us;          /* firmware monotonic clock */
  } alp_cc3501e_ping_reply_t;
  ```

  with

  ```c
  typedef enum {
      ALP_CC3501E_RESET_UNKNOWN  = 0,
      ALP_CC3501E_RESET_POR      = 1, /* RSTN pin / POR */
      ALP_CC3501E_RESET_RVM      = 2, /* rail voltage monitor */
      ALP_CC3501E_RESET_BROWNOUT = 3,
      ALP_CC3501E_RESET_WDT      = 4,
      ALP_CC3501E_RESET_SELF     = 5, /* M33-initiated, post CMD_RESET */
      ALP_CC3501E_RESET_DEBUG    = 6,
  } alp_cc3501e_reset_cause_t;
  ```

  Pure additive — v1 has no PING reply payload defined, so this is
  forward-compatible. Old firmware sending an empty PING reply still
  parses (reset_cause defaults to UNKNOWN).

- **Add `ALP_CC3501E_CMD_POWER_POLICY` (opcode `0x04` in the meta
  range).** Single-byte payload:

  ```c
  typedef enum {
      ALP_CC3501E_POWER_LATENCY = 0, /* default, no sleep — wake-from-sleep < 1 ms */
      ALP_CC3501E_POWER_BALANCED = 1, /* sleep + latency budget */
      ALP_CC3501E_POWER_LOWEST   = 2, /* aggressive — wake costs more */
  } alp_cc3501e_power_policy_t;
  ```

  Forwarded to TI SimpleLink Power Manager on the firmware side; the
  host doesn't need to know the M33 state machine.

### 3.10 SimpleLink driver-host wire interface (NOT in SWRU626)

This is the chapter the maintainer flagged as MOST IMPORTANT, and the
finding from §2.3 is the load-bearing one: **SWRU626 does not contain
this content**. The "SimpleLink driver-host wire interface" is the
NetCP / NWP / `_SL_PROTOCOL_*` framing exchanged between TI's `sl_*`
host driver and the CC3501E's wireless network processor. It is
documented in the TI SimpleLink CC33xx SDK (separate doc set), not
in SWRU626.

For ADR-0005 reasons, alp-sdk **does not learn that protocol**. The
CC3501E firmware speaks it internally — TI's `sl_WlanProfileAdd`,
`sl_NetAppDhcpClientGet`, etc. — and translates each
`ALP_CC3501E_CMD_WIFI_*` from the Alif into the right `sl_*` call.

#### 3.10.1 What this means for the wire protocol

The wire protocol in `<alp/protocol/cc3501e.h>` is **deliberately
NOT a wrapper over `_SL_PROTOCOL_*`**. It's a small, hand-designed
binary protocol whose call shape matches `<alp/iot.h>` and
`<alp/ble.h>`. This was the right design choice and we should not
revisit it on the basis of "TI has a wire protocol already". The
benefits:

- The protocol stays Apache-2.0 clean (no TI BSD-3 + restricted-use
  text leaks into our headers).
- It stays small (we don't pay the cost of `_SL_PROTOCOL_*` framing
  on every transaction).
- It can express GPIO proxy + camera-enable + diagnostics in addition
  to Wi-Fi + BLE — `sl_*` covers only the wireless half.

#### 3.10.2 What it means for the firmware

The firmware engineer's mental model is:

1. Receive an Alif-side frame `<header (4 B), payload (≤ 512 B)>` over
   SPI1 (peripheral mode).
2. Dispatch on the opcode.
3. For Wi-Fi opcodes → translate payload fields → call `sl_Wlan*` /
   `sl_NetApp*` / `sl_Socket*` → translate the SimpleLink return value
   to `alp_cc3501e_resp_t`.
4. For BLE opcodes → call the TI BLE host API (Apache-2.0 portion of
   the SimpleLink SDK).
5. For GPIO opcodes → drive IOMUX directly (no TI driver needed).
6. For camera opcodes → drive GPIO_0 / GPIO_1.
7. Stage the response in the SPI TX FIFO + raise busy/ready GPIO (per
   §3.4.3).

#### 3.10.3 Open question for the maintainer

> Which SimpleLink CC33xx SDK release does the first
> `alplabai/cc3501e-firmware` tag pin to? The SDK ships
> per-CC35xx-family revision and the API stability story matters: if
> we pin to the latest at first-firmware time, we lock the BLE host
> + Wi-Fi driver API surface for that firmware lifetime. If we pin
> to LTS, we lag features. **No host-side impact either way** — the
> wire protocol is the buffer — but it affects how often we need
> `alplabai/cc3501e-firmware` v0.x bumps.

### 3.11 Boot, secure boot, OTA — SWRU626 §10 (pp. 1040–1047)

The chain of trust is 100% firmware-side, but a few facts about it
inform what `firmware/cc3501e/flash.py` needs to do.

#### 3.11.1 Image layout (SWRU626 §10.3.1)

External flash:

1. **Boot sector** at flash offset 0. Contains the configuration
   needed to drive the xSPI flash interface (clock div, dummy cycles,
   read mode).
2. **TI BL2** image — signed by TI; the only updatable bootloader.
3. **Wireless Connectivity image** — TI-signed FW that the M33 pushes
   to the wireless subsystem when Wi-Fi/BLE is enabled.
4. **Vendor image** — MCUboot-formatted (§10.3.2), signed with the
   vendor (Alp Lab) private key. The OTP-burned vendor root-of-trust
   is the hash of the public key.

#### 3.11.2 Initial programming + reprogramming (§10.4.3, §10.4.4)

Both flows require a "signed programming request" delivered over the
**debug interface**. SWRU626 implies this is JTAG/SWD direct to the
CC3501E debug pads. Per `docs/cc3501e-bridge.md`, the alp-sdk
`flash.py` proposes routing the image over the inter-chip SPI1 with
the Alif acting as a debug relay. **This is a non-trivial firmware-
side implementation** — TI's `uniflash` does the standard path; we'd
be writing a custom relay protocol.

> **Open question:** does `firmware/cc3501e/flash.py` ship the relay
> approach (SPI1 → Alif → CC3501E debug pads via JTAG bit-bang) or
> require a direct JTAG probe to CC3501E debug pads? The README hints
> at the relay approach but flags it as "STUB. Real implementation
> lands alongside the first prebuilt binary." This is a decision for
> the `alplabai/cc3501e-firmware` v0.1 milestone, not for v0.x of
> alp-sdk.

#### 3.11.3 ADR-0005 split

100% firmware-side. The only host-side artefact is `flash.py`
(driving the Alif's debug probe + relay), which is already in the
right place per ADR 0005 (`firmware/cc3501e/flash.py` — dual-use
helper).

#### 3.11.4 Wire-protocol additions for boot/OTA

- **Reserve `0xF0..0xFF` for OTA / bootloader opcodes.** Mirror the
  gd32-bridge protocol's convention (cf. `docs/gd32-bridge-protocol.md`
  §10 Path A — bootloader opcodes reserved 0xF0..0xFF, replying
  `STATUS_NOSUPPORT` until the FMC integration lands). This is purely
  reservation; do not allocate or implement at this commit.

---

## 4. Gap analysis vs current alp-sdk stub

### 4.1 `chips/cc3501e/cc3501e.c` — current shape

133 LOC. Coverage:

| Function                              | State            | Gap vs SWRU626 reality |
|---------------------------------------|------------------|------------------------|
| `cc3501e_init()`                      | shipped          | OK; no power-on side-effects |
| `cc3501e_reset()`                     | shipped, stubbed | Missing 10 µs reset-hold + 900 ms boot delay (SWRU626 §7.1.5 Table 7-3 + power-up sequencing note). Needs `alp_delay_*`. |
| `cc3501e_get_version()`               | shipped          | Reply assumed to be 2 bytes; should be the extended PING reply payload (§3.9.5). |
| `cc3501e_request()`                   | shipped, single-frame | No busy/ready GPIO yet — relies on firmware staging the reply within the same transaction. SWRU626 §18 doesn't require this; in practice the host needs the GPIO handshake (§3.1.4 Option A). |
| `cc3501e_set_event_callback()`        | shipped          | No async-event RX thread yet — callback registered but nothing fires it. |
| `cc3501e_deinit()`                    | shipped          | OK |

### 4.2 `<alp/chips/cc3501e.h>` — current public API surface

114 LOC. Current public surface (the names the user touches):

- Lifecycle: `cc3501e_init`, `cc3501e_reset`, `cc3501e_deinit`.
- Synchronous request: `cc3501e_request`.
- Convenience: `cc3501e_get_version`.
- Async events: `cc3501e_set_event_callback` + `cc3501e_event_cb_t`.
- Struct fields (`cc3501e_t`): `bus`, `enable_pin`, `reset_pin`,
  `event_cb`, `event_user`, scratch buffers.

**Gaps vs the integration plan above:**

1. No `busy_pin` field on `cc3501e_t`. Needed for §3.1.4 Option A.
2. No `alp_delay_*` dependency declared. Needed for §3.9.2 (reset
   hold + boot wait).
3. No `cc3501e_ping()` helper distinct from `cc3501e_get_version()`.
   With the proposed extended PING reply (§3.9.5), `_get_version()`
   is a strict subset of `_ping()` — keep both for backwards-compat
   but route `_get_version()` through `_ping()` internally.
4. No `cc3501e_set_power_policy()`. Needed once §3.9.5
   `ALP_CC3501E_CMD_POWER_POLICY` ships.
5. No event-thread spawn / pump. The current `set_event_callback()`
   is dormant. v0.4 should spawn an RX thread (Zephyr semaphore +
   work-queue or thread, OS-agnostic surface to be designed).

### 4.3 `<alp/protocol/cc3501e.h>` — current wire opcodes

235 LOC. Currently allocated:

| Opcode (hex) | Name                          | Direction         | Status |
|--------------|-------------------------------|-------------------|--------|
| `0x00`       | PING                          | host→fw, fw→host  | Allocated; reply payload undefined |
| `0x01`       | GET_VERSION                   | host→fw, fw→host  | Allocated; 2-byte reply |
| `0x02`       | RESET                         | host→fw           | Allocated |
| `0x03`       | GET_MAC                       | host→fw, fw→host  | Allocated |
| `0x10..0x17` | WIFI_SCAN/CONNECT/AP/RSSI/IP  | host→fw           | Allocated; struct for CONNECT_STA defined |
| `0x18..0x1A` | EVT_WIFI_*                    | fw→host           | Allocated; SCAN_RESULT struct defined |
| `0x20..0x24` | SOCK_OPEN/CONNECT/SEND/RECV/CLOSE | host→fw       | Allocated; no payload struct yet |
| `0x30..0x3B` | BLE_ENABLE..GATT_WRITE        | host→fw           | Allocated |
| `0x3C..0x3F` | EVT_BLE_*                     | fw→host           | Allocated; ADV_REPORT struct defined |
| `0x50..0x53` | GPIO_CONFIGURE/WRITE/READ/SET_INT | host→fw       | Allocated; CONFIGURE + WRITE structs defined; SET_INTERRUPT struct missing |
| `0x54`       | EVT_GPIO_INTERRUPT            | fw→host           | Allocated; payload struct missing |
| `0x60..0x61` | CAM_ENABLE/DISABLE            | host→fw           | Allocated |
| `0x70..0x71` | DIAG_GET_STATS / DIAG_LOG_LEVEL | host→fw         | Allocated; payload structs missing |

**Missing on the host side (today's stub):**

1. PING-reply payload struct (`alp_cc3501e_ping_reply_t`) + reset-cause
   enum (`alp_cc3501e_reset_cause_t`).
2. Named direction + pull enums for GPIO_CONFIGURE.
3. `alp_cc3501e_gpio_set_interrupt_t` struct.
4. `alp_cc3501e_gpio_interrupt_evt_t` struct (with timestamp_us).
5. `ALP_CC3501E_CMD_POWER_POLICY` opcode + `alp_cc3501e_power_policy_t`.
6. Payload structs for socket opcodes (SOCK_OPEN, SOCK_CONNECT,
   SOCK_SEND, SOCK_RECV, SOCK_CLOSE). Not strictly blocking, but the
   firmware needs them to write the parser deterministically.
7. Payload structs for BLE GATT_REGISTER, GATT_NOTIFY, GATT_READ,
   GATT_WRITE.
8. Payload structs for DIAG_GET_STATS / DIAG_LOG_LEVEL.
9. Reserved-range marker comments for the 4 unallocated future groups
   (`0xA0..0xAF` UART, `0xB0..0xBF` ADC, `0xC0..0xCF` Timer/Capture,
   `0xF0..0xFF` OTA/Bootloader). Pure documentation hygiene.

### 4.4 Handoff checklist for `alplabai/cc3501e-firmware`

When the firmware repo is created, every item in §3 that says
"firmware-side" is a contract obligation. Concrete checklist (each
item references the relevant SWRU626 section):

- [ ] SPI peripheral-mode setup (§18.5): `CTL0 / CTL1 / CLKCFG0 /
      CLKCFG1 / IFLS / DMACR / IMASK`. Target 8 MHz at start (matches
      `<alp/chips/cc3501e.h>` doc-comment default).
- [ ] SPI RX channel high-priority DMA (§11.3.8) at peripheral index 6.
- [ ] SPI peripheral `RXOVF` ISR (§18.3.3) — log + bump diag counter.
- [ ] SPI peripheral `RTOUT` (§18.3.3 / §18.3.4) as a liveness watchdog.
- [ ] Busy/ready GPIO drive (assignment TBD; §3.1.4 Option A).
- [ ] GPIO IOMUX configure-order workaround (§16.4 note).
- [ ] Open-drain GPIO emulation (§3.4.2).
- [ ] Edge-detect IRQ → AON event → NVIC → `EVT_GPIO_INTERRUPT` frame.
- [ ] Camera-enable GPIO_0 / GPIO_1 default-OFF (§3.4).
- [ ] Safe-default mux state at boot (per `docs/cc3501e-bridge.md`
      § "Safe-default mux state").
- [ ] Watchdog timer (§7.2.2) reset sequence + threshold.
- [ ] Brownout (§7.1.4) — confirm 1.71 V trip; report on next PING.
- [ ] Reset-cause read-back (§7.2.1, Table 7-4) → encode into
      `alp_cc3501e_reset_cause_t` for PING reply.
- [ ] TI SimpleLink Power Manager integration for
      `ALP_CC3501E_CMD_POWER_POLICY` (§3.9.5).
- [ ] MCUboot vendor image signing + boot-sector layout (§10.3).
- [ ] OTP root-of-trust burn during activation flow (§10.4.2). One-time
      per device.
- [ ] BL2 anti-rollback enforcement (§10.4.1 note).

---

## 5. Follow-up commit list (host-side)

These commits land in **alp-sdk**, on dedicated feature branches off
`main` (per CONTRIBUTING.md + `feedback_release_hook.md`-equivalent
release flow). Order matters: each later commit may depend on earlier
ones.

### 5.1 host-cc3501e-1 — Protocol docs hygiene + reserved-range markers

**Branch:** `feature/cc3501e-protocol-doc-hygiene`
**Files touched:**
- `include/alp/protocol/cc3501e.h` (comment-only changes).

**Changes:**
- Add reserved-range comments for `0xA0..0xAF` UART proxy, `0xB0..0xBF`
  ADC proxy, `0xC0..0xCF` Timer/Capture proxy, `0xF0..0xFF` OTA/
  Bootloader. Mark them all as "do not allocate at this commit".
- Add a one-paragraph "wire format vs TI SimpleLink wire format"
  note pointing at the §3.10 finding (alp-sdk wire ≠ `_SL_PROTOCOL_*`).

**Opcodes added:** none.
**Protocol-vector regen needed:** no.
**ABI bump:** no.
**CHANGELOG skeleton:**
```
### Changed
- `<alp/protocol/cc3501e.h>` — document reserved opcode ranges for
  future UART/ADC/Timer/OTA proxies (no behavioural change).
```

### 5.2 host-cc3501e-2 — Named enums for GPIO direction + pull

**Branch:** `feature/cc3501e-gpio-named-enums`
**Files touched:**
- `include/alp/protocol/cc3501e.h`
- `tests/zephyr/chips/src/main.c` (add a smoke test that the enum
  values match the historical magic numbers).

**Changes:**
- Add `alp_cc3501e_gpio_dir_t` + `alp_cc3501e_gpio_pull_t` enums.
- Re-document `alp_cc3501e_gpio_configure_t.direction` and `.pull`
  fields to reference the enums.

**Opcodes added:** none.
**Protocol-vector regen:** no.
**ABI bump:** no.
**CHANGELOG skeleton:**
```
### Added
- `<alp/protocol/cc3501e.h>` — `alp_cc3501e_gpio_dir_t` and
  `alp_cc3501e_gpio_pull_t` enums for type-safe GPIO proxy configure.
  Wire-compatible with v1.
```

### 5.3 host-cc3501e-3 — GPIO set-interrupt + event payload structs

**Branch:** `feature/cc3501e-gpio-interrupt-payload`
**Files touched:**
- `include/alp/protocol/cc3501e.h`
- `chips/cc3501e/cc3501e.c` (optional: helper `cc3501e_gpio_set_interrupt()`).
- `include/alp/chips/cc3501e.h` (matching public helper).
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Add `alp_cc3501e_gpio_set_interrupt_t` (4 B) and
  `alp_cc3501e_gpio_interrupt_evt_t` (16 B, includes `uint64_t
  timestamp_us`).
- Add a `cc3501e_gpio_set_interrupt()` host helper that wraps
  `cc3501e_request()` with the new payload.

**Opcodes added:** none (opcodes already allocated at v1; only payload
shape is new).
**Protocol-vector regen:** **yes** (new vectors for the GPIO interrupt
configure + event paths so the firmware-side parser can be unit-tested
against canonical byte streams).
**ABI bump:** no public-API removal; pure addition.
**CHANGELOG skeleton:**
```
### Added
- `<alp/protocol/cc3501e.h>` — `alp_cc3501e_gpio_set_interrupt_t`
  payload + `alp_cc3501e_gpio_interrupt_evt_t` event payload (carries
  firmware-side timestamp).
- `<alp/chips/cc3501e.h>` — `cc3501e_gpio_set_interrupt()` helper.
```

### 5.4 host-cc3501e-4 — Extended PING reply + reset cause

**Branch:** `feature/cc3501e-extended-ping`
**Files touched:**
- `include/alp/protocol/cc3501e.h`
- `chips/cc3501e/cc3501e.c` (extend `cc3501e_get_version()` parse to
  also populate `cc3501e_ping_info_t`; or add `cc3501e_ping()`).
- `include/alp/chips/cc3501e.h`.
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Add `alp_cc3501e_reset_cause_t` enum + `alp_cc3501e_ping_reply_t`
  struct (per §3.9.5).
- Add `cc3501e_ping(cc3501e_t *ctx, cc3501e_ping_info_t *info)` host
  helper.
- Keep `cc3501e_get_version()` working — it just reads the first 2
  bytes of the new reply. Backwards-compatible with old firmware
  (which sends only 2 bytes — extra fields default to 0).

**Opcodes added:** none.
**Protocol-vector regen:** yes (new PING reply vector).
**ABI bump:** no.
**CHANGELOG skeleton:**
```
### Added
- `<alp/protocol/cc3501e.h>` — `alp_cc3501e_ping_reply_t` extends the
  PING reply with reset-cause + firmware uptime.
- `<alp/chips/cc3501e.h>` — `cc3501e_ping()` helper.
```

### 5.5 host-cc3501e-5 — Boot timing + reset-hold

**Branch:** `feature/cc3501e-reset-timing`
**Files touched:**
- `chips/cc3501e/cc3501e.c`
- (Possibly) `src/delay_*.c` if `alp_delay_*` doesn't exist yet —
  separate prerequisite commit.
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Honour the 10 µs reset-hold (SWRU626 §7.1.5 power-up sequencing
  note).
- After releasing reset, wait 900 ms before issuing the first PING
  (SWRU626 §7.1.5 Table 7-3 boot budget T1+T2+T3+T4).
- Optionally make the post-reset wait poll for a successful PING
  with retries (early-success exit if firmware is faster than the
  worst-case T4).

**Opcodes added:** none.
**Protocol-vector regen:** no (timing change is host-side only).
**ABI bump:** no.
**CHANGELOG skeleton:**
```
### Fixed
- `chips/cc3501e/cc3501e.c` — `cc3501e_reset()` now honours the
  SWRU626 §7.1.5 10 µs reset-hold and 900 ms boot budget before the
  first PING attempt.
```

### 5.6 host-cc3501e-6 — Busy/ready GPIO handshake

**Branch:** `feature/cc3501e-busy-ready`
**Files touched:**
- `include/alp/chips/cc3501e.h` (add `busy_pin` field).
- `chips/cc3501e/cc3501e.c` (poll the pin in `cc3501e_request()`).
- `metadata/e1m_modules/aen/from-cc3501e.tsv` (when the pin
  assignment is decided — TBD).
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Add `alp_gpio_t *busy_pin` to `struct cc3501e`.
- In `cc3501e_request()`, after staging the request frame, poll
  `busy_pin` (with timeout) before reading back the response.

**Opcodes added:** none.
**Protocol-vector regen:** no.
**ABI bump:** **yes** — `struct cc3501e` grows a field. Public struct
sizes change. Per the existing ABI snapshot tooling, this needs a
`docs/abi/v0.4-snapshot.json` bump.
**CHANGELOG skeleton:**
```
### Added
- `<alp/chips/cc3501e.h>` — `cc3501e_t::busy_pin` for SPI handshake.
### Changed
- Public ABI bumped to v0.4 due to `struct cc3501e` field addition.
```

### 5.7 host-cc3501e-7 — Power-policy opcode

**Branch:** `feature/cc3501e-power-policy`
**Files touched:**
- `include/alp/protocol/cc3501e.h`
- `chips/cc3501e/cc3501e.c`
- `include/alp/chips/cc3501e.h`.
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Add `ALP_CC3501E_CMD_POWER_POLICY = 0x04`.
- Add `alp_cc3501e_power_policy_t` enum.
- Add `cc3501e_set_power_policy()` host helper.

**Opcodes added:** 1 (`0x04`).
**Protocol-vector regen:** yes.
**ABI bump:** no (no struct size change).
**CHANGELOG skeleton:**
```
### Added
- `<alp/protocol/cc3501e.h>` — `ALP_CC3501E_CMD_POWER_POLICY` opcode
  (0x04) and `alp_cc3501e_power_policy_t` enum.
- `<alp/chips/cc3501e.h>` — `cc3501e_set_power_policy()` helper.
```

### 5.8 host-cc3501e-8 — Socket + BLE payload structs

**Branch:** `feature/cc3501e-socket-ble-payloads`
**Files touched:**
- `include/alp/protocol/cc3501e.h`.
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Add `alp_cc3501e_sock_open_t / _connect_t / _send_t / _recv_t /
  _close_t` payload structs.
- Add `alp_cc3501e_ble_gatt_register_t / _notify_t / _read_t /
  _write_t` payload structs.
- Add diagnostic payload structs (`alp_cc3501e_diag_stats_reply_t`,
  `alp_cc3501e_diag_log_level_t`).

**Opcodes added:** none (opcodes were allocated at v1; only payloads
are new).
**Protocol-vector regen:** yes (extensive — every new payload).
**ABI bump:** no.
**CHANGELOG skeleton:**
```
### Added
- `<alp/protocol/cc3501e.h>` — payload structs for socket, BLE GATT,
  and diagnostic opcodes (previously documented as opcode-only).
```

### 5.9 host-cc3501e-9 — Async event RX thread (Zephyr backend first)

**Branch:** `feature/cc3501e-event-rx-thread`
**Files touched:**
- `chips/cc3501e/cc3501e.c`.
- `src/zephyr/cc3501e_event_rx.c` (new — Zephyr-backend RX thread).
- `include/alp/chips/cc3501e.h` (maybe — start/stop hooks).
- `tests/zephyr/chips/src/main.c`.

**Changes:**
- Spawn a Zephyr thread on first `cc3501e_set_event_callback()` call
  (or via explicit `cc3501e_start_event_pump()`).
- The thread blocks on the busy/ready GPIO interrupt (from commit 6)
  or polls SPI MISO at a low duty cycle until the firmware drops
  an async-event frame (`FLAG_ASYNC_EVENT` set).
- Dispatch to `ctx->event_cb`.

**Opcodes added:** none.
**Protocol-vector regen:** no.
**ABI bump:** maybe (if we add start/stop hooks to the public struct).
**CHANGELOG skeleton:**
```
### Added
- `chips/cc3501e/` — Zephyr-backed async-event RX thread.
  `cc3501e_set_event_callback()` is now live — previously the
  callback was registered but never invoked.
```

### 5.10 Commit ordering (dependency order)

```
5.1 (doc hygiene)             ─┐
                               ├── independent; can land in parallel
5.2 (named enums)             ─┘

5.3 (GPIO interrupt payload)   ── depends on 5.2 (uses the enums)

5.4 (extended PING)            ── independent

5.5 (reset timing)             ── independent  (but prereq for any
                                  test that actually exercises the
                                  real chip)

5.6 (busy/ready GPIO)          ── depends on a pin-assignment
                                  decision (TBD) — block on
                                  signal-integrity / pin allocator

5.7 (power policy)             ── independent

5.8 (socket + BLE payloads)    ── independent of host code; large
                                  protocol-vector regen — sequence
                                  after 5.3 and 5.4 to keep diffs
                                  reviewable

5.9 (event RX thread)          ── depends on 5.6 (the busy/ready
                                  GPIO is the natural RX-thread
                                  wakeup source)
```

**Recommended ordering:** 5.1 → 5.2 → 5.4 → 5.5 → 5.3 → 5.7 → 5.8 →
(pin assignment decided) → 5.6 → 5.9.

Items 5.1–5.5 are low-risk and can land before the first firmware
binary is available. Items 5.6 + 5.9 require either real silicon or
the firmware-side mock to validate.

---

## 6. Follow-up commit list (firmware-side, for `cc3501e-firmware`)

Each item references the SWRU626 chapter that governs it. Numbering
mirrors the host-side list where possible.

### 6.1 fw-cc3501e-1 — Repo skeleton + license + SDK submodule

Per `firmware/cc3501e/README.md` "Bootstrap checklist" steps 1–4.
Apache-2.0 license, TI SimpleLink CC33xx SDK vendored at
`vendor/simplelink-cc33xx/`, pinned to a specific TI release tag
(decision: §3.10.3 open question).

### 6.2 fw-cc3501e-2 — SPI peripheral parser

SWRU626 §18.5. Implements the `<header (4 B), payload>` parser.
Maps to alp-sdk commit `host-cc3501e-1` (mirror of the protocol
header).

### 6.3 fw-cc3501e-3 — GPIO IOMUX driver (alp-protocol surface)

SWRU626 §16. Implements `IOMUX.GPIOnPCFG/CFG/CTL/PCTL/ECTL`
programming with the configure-order workaround (§16.4 note), the
open-drain emulation (§3.4.2), and the edge-detect → AON-event →
NVIC → `EVT_GPIO_INTERRUPT` path. Mirrors host commits 5.2 + 5.3.

### 6.4 fw-cc3501e-4 — Power manager + reset-cause read-back

SWRU626 §7. Reads `Reset Cause` (§7.2.1, Table 7-4), populates the
extended PING reply, integrates TI SimpleLink Power Manager for the
`POWER_POLICY` opcode. Mirrors host commit 5.4 + 5.7.

### 6.5 fw-cc3501e-5 — Boot timing + nRESET hold

SWRU626 §7.1.5. **Note:** the 10 µs nRESET hold and 900 ms boot
budget are HOST-SIDE timing; the firmware side just has to be ready
to PING within 900 ms (it has BL1 + BL2 + Chain-of-Trust + application
init within that window). This is implicit but worth a checklist item:
the firmware must initialise the SPI peripheral + GPIO contract within
T4.

### 6.6 fw-cc3501e-6 — Busy/ready GPIO driver

Mirror of host commit 5.6. Firmware drives any free GPIO low during
RX/dispatch + reply staging, releases when TX FIFO is loaded.

### 6.7 fw-cc3501e-7 — Wi-Fi opcode routing to `sl_Wlan*`

SWRU626 §18 covers the SPI path. The Wi-Fi opcodes route to TI
SimpleLink's `sl_Wlan*` / `sl_NetApp*` APIs. SWRS343 confirms STA,
softAP, Wi-Fi-direct, multi-role AP + STA support.

### 6.8 fw-cc3501e-8 — BLE opcode routing to TI's BLE host

SWRS343 § Bluetooth low energy: BLE 5.4 certified stack, long-range
+ high-speed PHYs up to 2 Mbps.

### 6.9 fw-cc3501e-9 — Camera-enable + GPIO proxy completion

Drives GPIO_0 / GPIO_1 for CAM_EN_LDO0/1 (per `inter-chip.tsv`),
proxies the M.2 + BMI323 + mux pins per
`docs/cc3501e-bridge.md` § "Firmware-side GPIO behaviour contract".

### 6.10 fw-cc3501e-10 — MCUboot vendor image signing + OTA path

SWRU626 §10. MCUboot vendor image format, OTP root-of-trust burn
(activation flow §10.4.2), reprogramming flow (§10.4.4),
anti-rollback. Maps to alp-sdk `firmware/cc3501e/flash.py` real impl.

---

## 7. Open questions for maintainer

These are listed in priority order. Each blocks at least one commit
in §5 or §6.

### 7.1 SimpleLink CC33xx SDK pin

Which SDK release does `alplabai/cc3501e-firmware` v0.1 pin to? See
§3.10.3. **Recommendation:** target latest stable at the time the
firmware repo is created; revisit per firmware-repo release. No
host-side impact.

### 7.2 GPIO direction enum values

Today the protocol uses magic numbers in `alp_cc3501e_gpio_configure_t.
direction` (`0 = input, 1 = output`). Commit 5.2 proposes adding
`2 = open-drain` and reserving `3 = open-drain-with-pullup`. Is the
two-value historical set authoritative, or can we extend to four
values? **Recommendation:** extend; the firmware doesn't ship yet so
the four-value enum is the first canonical version.

### 7.3 Host-side SPI speed: dual-tier or fixed?

Today `<alp/chips/cc3501e.h>` doc-comment uses 8 MHz. SWRU626 §18
allows the SPI peripheral to clock at "max SPI frequency depends on
device clock option and IO option — see device datasheet" — SWRS343
caps at the IO ring capability for VIO1 / VIO2 (TBD: confirm in the
errata, SWRZ167). Two paths:

- **(A) Fix at 8 MHz for v0.x.** Simpler. Plenty of headroom for the
  per-frame opcodes; tight for raw socket throughput.
- **(B) Add a high-speed mode (e.g. 26 MHz) gated by signal-integrity
  validation.** Doubles bus throughput.

**Recommendation:** ship (A) at v0.x. Move to (B) only if socket
throughput becomes a measured bottleneck; defer to v1.x.

### 7.4 Busy/ready GPIO pin assignment

§3.1.4 Option A + §3.4.3. Needs a free CC3501E GPIO routed to a free
Alif pad on the E1M-AEN module. The pin allocator + signal-integrity
review owns this. **Blocking commit 5.6 (and therefore 5.9).** No
firmware blocking — the firmware can implement the driver against
any GPIO and the pin assignment is a build-time config.

### 7.5 SDIO vs SPI for v1.x

The hardware supports both. The wire protocol is transport-agnostic.
Do we ship SDIO support at v1.x or wait for a measured bottleneck?
**Recommendation:** keep SPI as the canonical path, defer SDIO host
surface (`alp_sdio_*`) until there's evidence of need.

### 7.6 OTA path for `flash.py`

§3.11.2 open question. Relay over Alif/SPI1 vs direct JTAG to CC3501E
debug pads. **Owner:** `alplabai/cc3501e-firmware` v0.1 milestone, not
this repo.

### 7.7 CC35xx errata SWRZ167

Not deep-read in this pass. The CC355xE errata is published
December 2025. **Action:** when the firmware repo opens, fold the
errata findings into the firmware-side checklist (§4.4); update this
plan with any host-visible errata items.

### 7.8 CAN, I2S, PDM, SDMMC — proxied or not?

CC3501E has them per SWRS343. Today none are routed to E1M pads via
`from-cc3501e.tsv`. **Recommendation:** stay off the wire protocol
unless a future E1M-AEN revision routes one of them. Reserve a low-
priority opcode range only if/when needed.

---

## 8. References

### 8.1 alp-sdk in-repo

- [`include/alp/chips/cc3501e.h`](../include/alp/chips/cc3501e.h) —
  host driver public API.
- [`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h) —
  wire protocol header (v1).
- [`chips/cc3501e/cc3501e.c`](../chips/cc3501e/cc3501e.c) — host
  driver implementation.
- [`docs/cc3501e-bridge.md`](cc3501e-bridge.md) — bridge architecture
  + firmware GPIO contract.
- [`docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md`](adr/0005-alp-sdk-vs-alp-studio-boundary.md)
  — split-repo rationale.
- [`docs/soms/aen.md`](soms/aen.md) — E1M-AEN family overview.
- [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv)
  — Alif ↔ CC3501E pin map.
- [`metadata/e1m_modules/aen/from-cc3501e.tsv`](../metadata/e1m_modules/aen/from-cc3501e.tsv)
  — E1M pad → CC3501E pin map.
- [`firmware/cc3501e/README.md`](../firmware/cc3501e/README.md) —
  firmware-side bootstrap.
- [`firmware/cc3501e/flash.py`](../firmware/cc3501e/flash.py) —
  flasher CLI shape.
- [`firmware/cc3501e/protocol-version.txt`](../firmware/cc3501e/protocol-version.txt)
  — wire-protocol pin (currently `1`).
- [`tests/zephyr/chips/src/main.c`](../tests/zephyr/chips/src/main.c)
  — CC3501E Ztest harness.

### 8.2 TI public documents

- **SWRU626** — CC35xx SimpleLink™ Wi-Fi 6 and Bluetooth® Low Energy
  Wireless MCU Technical Reference Manual, December 2025.
  <https://www.ti.com/lit/ug/swru626/swru626.pdf>
- **SWRS343** — CC350xE 2.4 GHz SimpleLink Wi-Fi 6 and Bluetooth Low
  Energy Wireless MCU Datasheet (CC3500E + CC3501E), December 2025.
  <https://www.ti.com/lit/ds/symlink/cc3501e.pdf>
- **SWRZ167** — CC355xE Errata, December 2025 (referenced; not deep-
  read in this pass). <https://www.ti.com/lit/er/swrz167/swrz167.pdf>
- **SLUUDH6** — CC35xx Hardware Integration, April 2026 (referenced;
  not deep-read). <https://www.ti.com/lit/ug/sluudh6/sluudh6.pdf>
- **TI SimpleLink Wi-Fi SDK** (CC33xx family) — Resource Explorer
  link in `docs/cc3501e-bridge.md` (login + click-through required).

### 8.3 Sibling-bridge precedent (in-repo, used as structural reference only)

- [`docs/gd32-bridge.md`](gd32-bridge.md) — analogous coprocessor-side
  doc for the GD32G553 on E1M-V2N. Used to confirm reserved-opcode
  conventions, build/flash flow surface area, and bootloader-opcode
  range reservation (mirrored in §3.11.4 above).
