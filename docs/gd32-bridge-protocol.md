# GD32 bridge protocol (V2N supervisor MCU)

> **Scope.** This document is the **wire** specification for the
> Renesas RZ/V2N ⇄ GD32G553MEY7TR bridge on the E1M-X V2N / V2N-M1
> SoMs.  Both sides — the host driver under
> `chips/gd32g553/` and the GD32-side firmware under `gd32-bridge/` —
> implement what is described here.  Bit, byte and timing decisions
> live in this file; the host-side public API is documented under
> `<alp/chips/gd32g553.h>`.
>
> **Authoritative carrier wiring** is in
> [`metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`](../metadata/e1m_modules/v2n/renesas-peripheral-map.tsv)
> (`GD32_SPI.*` rows + `BRD_I2C` rows).  See also
> [memory/project_gd32_bridge_hybrid_spi_i2c.md](../memory/project_gd32_bridge_hybrid_spi_i2c.md).

## 1. Hardware transports

The GD32 supervisor is reachable on V2N over **two parallel physical
buses**.  The bridge speaks the **same command-set** on both — only
the framing layer differs.

| Property              | SPI (fast path)                                                            | I2C (management path)                          |
|-----------------------|----------------------------------------------------------------------------|------------------------------------------------|
| Master                | Renesas RZ/V2N RSPI                                                        | Renesas RZ/V2N RIIC8 (BRD_I2C master)          |
| Slave                 | GD32G553 SPI peripheral                                                    | GD32G553 I2C peripheral                        |
| Renesas pads          | `P76` MOSI, `P77` MISO, `P96` SCLK, `P97` CS                               | `P07` SCL, `P06` SDA                           |
| GD32 pads             | `PB15` MOSI, `PA10` MISO, `PA9` SCLK, `PA8` CS                             | `PA15` SCL, `PB9` SDA (GD32 = bus slave)       |
| Speed                 | ≤ 25 MHz (host can step down for noisy boards; firmware caps at SoC limit) | Standard / Fast / Fast+ (host picks)           |
| Mode                  | SPI mode 0 (CPOL=0, CPHA=0), MSB-first, 8-bit words                        | 7-bit addressing                               |
| Bus arbitration       | GD32 is dedicated slave — no other masters on the SPI link                 | BRD_I2C is shared (PMICs, RTC, OPTIGA, …)      |
| Liveness signalling   | CS edge transitions                                                        | START / STOP envelope                          |

The host driver can **route a given command over whichever
transport is open** at call time.  Carriers that only wire one of the
two transports work unchanged; commands on the un-wired transport
return `ALP_ERR_NOSUPPORT`.

## 2. Endianness and integers

All multi-byte integers are **little-endian** on the wire.  Where a
field is declared `uint32_t` in the C structs, byte 0 is the LSB.

## 3. Common command set

Both transports carry the same command set.  Command opcodes are 1
byte; their numeric encoding is:

| Opcode | Name                  | Payload (request)                                  | Reply payload                                      |
|--------|-----------------------|----------------------------------------------------|----------------------------------------------------|
| `0x00` | `PING`                | _empty_                                            | `0x00` (`ALP_OK`)                                  |
| `0x01` | `GET_VERSION`         | _empty_                                            | `major:u8 minor:u8 patch:u8` (`major.minor.patch`) |
| `0x02` | `GET_BUILD_ID`        | _empty_                                            | `build_id:char[20]` (truncated SHA-1, ASCII hex)   |
| `0x03` | `RESET_REASON`        | _empty_                                            | `cause:u8` (`gd32g553_reset_cause_t`)              |
| `0x10` | `GPIO_READ`           | `mask:u32`                                         | `levels:u32` (masked subset)                       |
| `0x11` | `GPIO_WRITE`          | `mask:u32 levels:u32`                              | _empty_                                            |
| `0x20` | `PWM_SET`             | `channel:u8 reserved:u8 period_ns:u32 duty_ns:u32` | _empty_                                            |
| `0x21` | `PWM_GET`             | `channel:u8`                                       | `period_ns:u32 duty_ns:u32`                        |
| `0x30` | `ADC_READ`            | `channel:u8 samples:u8`                            | `mv[samples]:u16` (millivolt, raw averaged)        |
| `0x40` | `DA9292_STATUS_FORWARD` | _empty_                                          | `da9292_status:u8` (latest cached PMC_STATUS_00)   |

Opcodes `0x80..0xFF` are **reserved** for vendor extensions.
The GD32 firmware replies with **`ALP_ERR_NOSUPPORT`** (see §6) for
any opcode it does not implement at build time, so a host that
speaks a newer command set than the firmware degrades gracefully.

### 3.1 GPIO masks

`mask` selects which GD32 pads the host wants to read or write.  The
mask is a **logical** index space owned by the GD32 firmware — the
bit-to-pad mapping is documented in `gd32-bridge/README.md` and
mirrored in the host driver header.  The host MUST NOT assume that
bit `n` corresponds to GD32 pad `Pxn`.

`GPIO_WRITE` is atomic in the firmware: read-modify-write of the
pad output register is done with interrupts disabled around the
masked update so a concurrent `GPIO_WRITE` on the other transport
cannot interleave a partial state.

### 3.2 PWM channels

PWM channel ids are an **opaque enum** assigned by the GD32 firmware.
Mapping to GD32 timer + GTIOC pad lives in the firmware's
`pwm_channel_map[]`.  Where the V2N module exposes both a Renesas
PWM route and a GD32 PWM route for the same E1M signal (E1M PWM6 +
PWM7 since the 2026-05-11 schematic revision), the **GD32 path is the
authoritative one** for the V2N base SoM.

### 3.3 ADC samples

`samples = 0` is invalid (reply: `ALP_ERR_INVAL`).  `samples >
GD32G553_BRIDGE_ADC_MAX_SAMPLES` (firmware-defined, currently 8) is
capped to the maximum.  Each `mv[i]` carries the firmware's
internal-reference-corrected reading; the host treats the values as
**ground truth** for telemetry purposes.

### 3.4 DA9292 status forward

The GD32 keeps a **cached** copy of the DA9292's PMC_STATUS_00 byte
populated by periodic I2C polls on its own loop.  The host receives
the most recent cached value with sub-millisecond latency and never
has to contend with the PMIC over the same I2C bus.  Cache age is
firmware-implementation-defined (currently ≤ 20 ms).

The cached byte uses the same bit layout as the on-chip register
(`DA9292_STATUS00_CH1_PG = bit 0`, …) — see `<alp/chips/da9292.h>`.

## 4. SPI framing

Each command on SPI is a **request frame** sent by the host while CS
is asserted, followed by a **reply frame** that the GD32 firmware
clocks back out on the *next* CS transaction within
`GD32G553_BRIDGE_REPLY_TIMEOUT_MS` (default 10 ms).

```
                                 1 byte  1 byte    N bytes     2 bytes
                                 ┌──────┬───────┬───────────┬─────────┐
   REQ  (host  → GD32)           │ SOF  │  CMD  │  PAYLOAD  │   CRC   │
                                 └──────┴───────┴───────────┴─────────┘
                                 1 byte  1 byte    M bytes     2 bytes
                                 ┌──────┬───────┬───────────┬─────────┐
   REPLY (GD32 → host)           │ SOF  │ STATUS│  PAYLOAD  │   CRC   │
                                 └──────┴───────┴───────────┴─────────┘
```

| Field   | Width | Notes                                                                                          |
|---------|-------|------------------------------------------------------------------------------------------------|
| SOF     | 1     | `0xA5`. Anything else → host or firmware abandons the frame and resyncs on the next CS edge.   |
| CMD     | 1     | Opcode from §3.                                                                                |
| STATUS  | 1     | Reply only.  `0x00` = OK.  See §6 for non-zero values.                                         |
| PAYLOAD | N / M | Length is **opcode-derived** — both ends know the byte count from the opcode + status pair.   |
| CRC     | 2     | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, xor-out 0x0000, **non-reflected**), MSB first |

Length is **not** carried on the wire because a single opcode has a
fixed request-payload width and a status-code-determined reply-payload
width — keeping the envelope at 1+1+N+2 bytes minimises the
fast-path overhead and avoids ambiguity at the SPI boundary
(SPI doesn't have a natural "end-of-frame" token).

Variable-length replies (`ADC_READ`, …) carry the length as the
first byte of the payload (`samples` for `ADC_READ`) — same byte the
host already sent in the request, **echoed back** before the data.

### 4.1 SPI timing

SPI uses a **two-transaction** pattern:

1. **Request transaction.**  Host asserts CS, clocks the request
   envelope out, deasserts CS.  This is a single half-duplex write
   (no useful data on MISO).
2. **Inter-transaction gap.**  Host-side overhead between the two
   bus calls is typically ≥ tens of microseconds — enough for the
   GD32 ISR to decode the request, run the handler, and stage the
   reply envelope in its TX FIFO.  Hosts that operate with a much
   faster bus driver (e.g. DMA-backed back-to-back transactions
   under 1 µs) MUST insert an explicit busy-wait of at least
   100 µs before issuing the read.
3. **Reply transaction.**  Host asserts CS, clocks 0xFF bytes out,
   reads the reply envelope on MISO, deasserts CS.

The pattern is **half-duplex** on each transaction (request: host
talks; reply: GD32 talks).  Single-CS full-duplex is not used
because the slave cannot pre-populate its TX FIFO before it has
decoded the opcode.  A future v2 protocol revision may add a
single-CS variant with fixed inter-phase padding bytes for
latency-critical use cases.

If the GD32 ISR has not finished staging the reply by the time the
host begins the reply transaction, the host reads the **stale**
contents of the GD32's TX FIFO (typically `0xFF` or the previous
reply's CRC).  The CRC check then fails and the driver returns
`ALP_ERR_IO` — callers can retry safely because the request side has
already executed (commands are idempotent except for `GPIO_WRITE` /
`PWM_SET`, where the host knows the desired final state and writing
the same value twice is benign).

### 4.2 CRC validation

* On bad CRC the receiver returns `ALP_ERR_IO` and *does not* execute
  the command body.
* On a bad SOF the receiver returns `ALP_ERR_IO` and waits for the
  next CS-low edge before sampling again.
* Reply CRCs are computed over `SOF | STATUS | PAYLOAD`.
* CRC-16/CCITT-FALSE is identical to the variant Zephyr's
  `crc16_itu_t(0xFFFF, …)` produces and to Python
  `crcmod.predefined.PredefinedCrc('xmodem-falseinit')` —
  reference vector: CRC over the ASCII string "123456789" =
  `0x29B1`.

## 5. I2C framing

I2C carries the **same opcode + payload + CRC envelope**, but the
SOF byte is replaced by the I2C `START + slave address` envelope,
and the **register-style addressing** familiar to bus-management
code is used so the bridge looks like a regular I2C slave to
discovery tools.

The GD32 firmware presents itself at a **single 7-bit slave
address** configured at compile time (default `0x70`; carriers can
override).  Inside that slave address space the firmware exposes
**one virtual register** through which all commands flow:

```
   write transaction (host → GD32)
   ┌──────────┬──────┬──────┬──────────┬─────────┐
   │ S+ADDR+W │ 0x00 │  CMD │ PAYLOAD  │   CRC   │
   └──────────┴──────┴──────┴──────────┴─────────┘

   read transaction (host → GD32)
   ┌──────────┬──────┬──────────┬─────────┬─────────┐
   │ S+ADDR+R │STATUS│ PAYLOAD  │   CRC   │   P     │
   └──────────┴──────┴──────────┴─────────┴─────────┘
```

The leading `0x00` register-address byte is the bridge's **command
register**; it exists only so the I2C framing matches the dominant
"write reg-addr then data" idiom on BRD_I2C.  All write transactions
write to register `0x00`; the firmware does not expose any other
register.

After a `write`, the host issues a `repeated-start read` (or a
fresh `read`) to consume the reply.  The reply payload length is
the same opcode-derived value as on SPI — but for I2C, where the
slave **can hold the bus** by clock-stretching, the firmware
guarantees that the reply bytes are available before it releases
SCL.  Hosts that don't support clock-stretching can poll the bus
busy bit instead.

If the host issues a `read` before any `write` since the last
START, the firmware replies with one byte `STATUS = 0x80` (no
pending command) and an empty payload + CRC.

### 5.1 CRC on I2C

The CRC of an I2C transaction covers `CMD | PAYLOAD` on write and
`STATUS | PAYLOAD` on read — the I2C address byte is NOT included,
matching the convention used by most smart-battery / SMBus PEC
protocols.  The polynomial and parameters are the same as SPI
(CRC-16/CCITT-FALSE), so both transports share one
verification routine in `gd32-bridge/src/protocol.c`.

### 5.2 Slave-address overlap

The default GD32 slave address `0x70` is **not** occupied by any
chip on the V2N BRD_I2C bus (verified against
`metadata/e1m_modules/E1M-V2N101/som.yaml`).  When a carrier
allocates BRD_I2C to a device that conflicts, the firmware is
rebuildable with a different `CONFIG_GD32G553_BRIDGE_I2C_ADDR` —
host code reads back the address from `GET_VERSION` reply payload
in future protocol revisions (TODO; today the host has to know the
configured address out-of-band).

## 6. Status codes (firmware → host)

The status byte returned in every reply maps 1:1 onto a subset of
the host-side `alp_status_t` enum.  The wire encoding is the
**absolute value** of the negative-numbered host enum so the status
byte is naturally unsigned and human-readable on a logic analyser:

| Wire `STATUS` | Host `alp_status_t`     | Meaning                                                |
|---------------|-------------------------|--------------------------------------------------------|
| `0x00`        | `ALP_OK`                | Command executed successfully.                         |
| `0x01`        | `ALP_ERR_INVAL`         | Bad arguments (e.g. channel out of range).             |
| `0x02`        | `ALP_ERR_NOT_READY`     | Sub-resource (PWM, ADC, DA9292 link) not initialised.  |
| `0x03`        | `ALP_ERR_BUSY`          | Firmware busy servicing a long-running operation.      |
| `0x04`        | `ALP_ERR_TIMEOUT`       | Sub-bus operation timed out (e.g. PMIC I2C).           |
| `0x05`        | `ALP_ERR_IO`            | CRC failure or transport-layer error.                  |
| `0x06`        | `ALP_ERR_NOSUPPORT`     | Opcode unknown to this firmware build.                 |
| `0x07`        | `ALP_ERR_NOMEM`         | Reserved.                                              |
| `0x08`        | `ALP_ERR_OUT_OF_RANGE`  | Parameter beyond hardware capability (e.g. PWM freq).  |
| `0x80`        | _(I2C: no pending cmd)_ | I2C read before any write on this START — see §5.      |
| Other         | `ALP_ERR_IO` (mapped)   | Unknown wire status → host returns `ALP_ERR_IO`.       |

Hosts MUST translate the wire byte back to a negative `alp_status_t`
via the table above before returning it from a public API call.

## 7. Liveness handshake

`gd32g553_init()` issues:

1. `PING` with a 100 ms timeout — confirms the link physically
   answers and the firmware is not wedged.
2. `GET_VERSION` — caches the major.minor.patch into the driver
   context.  If `major` mismatches `GD32G553_HOST_PROTOCOL_MAJOR`,
   the init returns `ALP_ERR_NOSUPPORT` and refuses to operate on
   incompatible firmware (avoids a host that speaks newer commands
   talking past an older firmware build).
3. `GET_BUILD_ID` (optional, only if the host logs it) — useful in
   production-test logs to confirm the GD32 has the firmware build
   that QC signed off on.

There is **no periodic keep-alive** between host and bridge.  The
host issues commands on demand; the bridge always replies.  The
host can detect a wedged bridge by issuing `PING` whenever the
caller wants liveness confirmation.

## 8. Backward compatibility policy

* The protocol carries a `major.minor.patch` version returned by
  `GET_VERSION`.
* `major` is bumped on **wire-breaking** changes (frame layout,
  CRC algorithm, command renumbering).
* `minor` is bumped when **opcodes are added** that older hosts
  don't have to know about.
* `patch` is bumped on documentation or non-observable firmware
  changes.

Until the bridge fleet ships in production, `major` stays at `0`
and the host driver insists on **exact** major-number match.

## 9. Reference vectors

The canonical sanity-check for the CRC implementation is the
universally-cited CRC-16/CCITT-FALSE result over the ASCII string
"123456789":

* `crc16_ccitt_false("123456789") == 0x29B1`

The per-opcode wire vectors (SPI `PING` round-trip, I2C `PING`
round-trip, `GET_VERSION` reply for the firmware's declared
version) are generated at firmware build time and stored in
`gd32-bridge/tests/protocol_vectors.txt`.  Both the host-side
driver tests under `tests/zephyr/chips/gd32g553/` and the
firmware-side unit tests under `gd32-bridge/tests/` consume
that file so the two implementations cannot diverge.

## 10. Field upgrades of the bridge firmware

> **Status: design committed; implementation pending.**

Two upgrade paths.  Per the V2N hardware decision (2026-05-12),
the carrier routes `GD32_SWDIO` + `GD32_SWCLK` + `GD32_NRST` from
the Renesas host to the GD32; the BOOT0-strap / factory-ISP path
was dropped after the GD32G553 boot ROM was confirmed
USART-only (User Manual Rev1.2 §1.4).  See
[memory/project_gd32_boot0_to_v2n_planned.md](../memory/project_gd32_boot0_to_v2n_planned.md)
for the design rationale.

**Path A — Application bootloader over the bridge (preferred
normal upgrade path).**

* Lives in the first N KiB of GD32 flash, never overwritten by a
  field upgrade.
* Implements an additional set of bridge opcodes (`OTA_BEGIN` /
  `OTA_WRITE_CHUNK` / `OTA_VERIFY` / `OTA_COMMIT` / `OTA_ROLLBACK`,
  numerically **reserved at `0xF0..0xFF`** in this protocol for the
  next minor revision).
* Keeps two **slots** in upper flash; the active slot runs at boot
  while the inactive slot receives the upgrade.  Roll-back is a
  metadata flip + reset.
* Uses the same SPI / I2C transport as the rest of the protocol --
  no extra wiring beyond what is already in place.

**Path B — Host-driven SWD bit-bang (universal recovery).**

* Used when Path A is unreachable (corrupt application bootloader,
  factory first-flash, dev-board bring-up).
* The Renesas host implements a software SWD controller, drives
  `GD32_SWDIO` + `GD32_SWCLK` as GPIOs, optionally asserts
  `GD32_NRST` to halt the GD32 cleanly, then issues SWD packets
  to reflash the entire chip.
* Works **regardless of GD32 firmware state** -- SWD is a hardware
  debug bus and doesn't depend on the boot ROM or any firmware
  layer being intact.
* Strictly more capable than the GD32's factory-ISP route, which
  would have required wiring a USART pair as well as BOOT0 +
  NRST.  Routing SWD is the cleaner design.
* Open-source reference implementations of bit-bang SWD: PyOCD,
  OpenOCD, J-Link OB.  ~500-1000 LOC for the protocol layer +
  GPIO HAL.
* Security note: any V2N firmware with access to the SWD GPIOs
  can reflash the GD32 unconstrained.  Same threat surface as
  Path A's OTA opcodes.

Until either path ships, **GD32 field upgrades go through an
external SWD probe** attached to the V2N module's programming
header.

## 11. Out-of-scope

* OTA of the **bridge firmware** beyond what §10 describes -- the
  device-side OTA contract for the Renesas-side firmware (the
  bigger one Mender drives) lives in
  [`docs/ota.md`](ota.md) +
  [`docs/ota-device-contract.md`](ota-device-contract.md); Hakan
  owns the server side.
* PMC events / PMIC alarms — the GD32 monitors but does not relay
  asynchronously over the bridge; the host polls via
  `DA9292_STATUS_FORWARD` when it wants the cached value.
* Streaming workloads (audio, video) — not in scope; use the
  Renesas direct peripherals for those.

## See also

* `<alp/chips/gd32g553.h>` — host-side public API.
* `gd32-bridge/README.md` — firmware-tree overview.
* `chips/gd32g553/gd32g553.c` — Renesas-side driver.
* `gd32-bridge/src/protocol.c` — shared command-handler table.
* `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — GD32 pad
  allocation.
