# 0016. Cross-core peripheral proxy wire schema (A55 → M33 → GD32 bridge)

Status: Proposed
Date: 2026-06-14

Forward design for the cross-core peripheral proxy flagged in issue #33's
"Cross-core proxy" section.  Pins the A55↔M33 wire contract so the Linux
proxy backends and the (not-yet-written) M33 dispatcher firmware can be built
independently against the same schema.  `Proposed` until the M33 dispatcher
lands and the loop is validated on real E1M-V2N-M1 silicon.

## Context

On **E1M-V2N-M1** the analog/timer peripherals exposed by `<alp/adc.h>`,
`<alp/counter.h>`, `<alp/pwm.h>`, `<alp/dac.h>` and the QEnc surface physically
live on the **GD32 bridge MCU**, behind the **M33 supervisor**.  The Renesas
RZ/V2N A-cluster (where Yocto/Linux runs) has no direct path to them.

Today:

- The Zephyr **M33** side already drives the GD32 over SPI via the
  `gd32g553_*` host driver, serialized through `alp_z_v2n_supervisor_acquire/
  _release` (`src/zephyr/v2n_supervisor.{c,h}`); the in-core registry backends
  `src/backends/{adc,counter,pwm,qenc}/gd32_bridge.c` and the DAC backend
  (`src/backends/dac/gd32_bridge.c`, ADR-prerequisite landed in PR #145) ride it.
- The **A55/Linux** side has a complete RPMsg transport: `src/backends/rpc/
  yocto_drv.c` binds `<alp/rpc.h>` onto the kernel RPMsg chardev
  (`/dev/rpmsg_ctrl0` + `RPMSG_CREATE_EPT_IOCTL`), with a synchronous
  `alp_rpc_call(ch, method, req, req_len, resp, resp_len, timeout_ms)`.  The
  rpc payload is **opaque** (`method\0payload`); the SDK deliberately does not
  parse it — the application defines its own sub-schema (`rpc.h` §framing).
- The backend selector already supports a V2N-M1-specific proxy: a backend
  registering `silicon_ref="renesas:rzv2n:n44"` at priority 100 **beats** the
  generic Linux `yocto_drv` (`"*"`, priority 100) by exact-match-over-wildcard
  (`src/backend.c:62-104`).  No engine change needed.

The **one missing piece** is the join: nothing today wires an RPMsg subscribe
to a `gd32g553_*` call.  Building that M33 firmware — and the matching Linux
proxy backends — requires both ends to agree on a wire schema.  This ADR pins it.

## Decision

**1. The proxy payload IS the GD32 bridge command frame, minus transport framing.**
RPMsg already provides framing + integrity, so the GD32 `SOF(0xA5)` + `CRC16`
are dropped:

- **Request payload:** `CMD:u8 | <GD32 request payload>` — the exact GD32 opcode
  (`firmware/gd32-bridge/src/protocol.h`, e.g. `PWM_SET 0x20`, `ADC_READ 0x30`,
  `COUNTER_READ 0x70`, `QENC_READ 0x60`, `DAC_SET 0x50`) followed by its
  documented payload, little-endian.
- **Reply payload:** `STATUS:u8 | <GD32 reply payload>` — the GD32 status code
  (low nibble; the SPI-only high-nibble `STATUS_SEQ` stamp is dropped, RPMsg is
  in-order) followed by the documented reply payload.

This makes the M33 a **near-thin relay**: unwrap the rpc payload → hand
`(CMD, payload)` to the GD32 bridge command layer (which builds the SOF/CRC
frame and transacts over SPI) → relay the `(STATUS, reply)` back over rpc.  No
opcode translation, no second schema to maintain — **the proxy *is* the GD32
protocol over a different transport**, and tracks its version automatically.

**2. One RPMsg method, synchronous request/response.**  A single method name
`"gd32"` carries every peripheral op; the `CMD` byte discriminates.  Use
`alp_rpc_call` (send + await the correlated reply).  Rationale: one endpoint,
and the GD32 link is serial regardless.

**3. Addressing lives in the GD32 payload.**  Every GD32 op already carries its
channel/instance (`PWM channel:u8`, `ADC channel:u8`, `QENC encoder:u8`, …), so
the proxy adds no addressing layer.

**4. Error mapping.**  The Linux proxy maps the GD32 `STATUS` byte → `alp_status_t`
with the same table the in-core `gd32_bridge` backends use (`STATUS_OK→ALP_OK`,
`STATUS_INVAL→ALP_ERR_INVAL`, `STATUS_NOSUPPORT→ALP_ERR_NOSUPPORT`, …).
`alp_rpc_call`'s `timeout_ms` bounds the round trip; a timeout → `ALP_ERR_TIMEOUT`.

**5. Concurrency.**  The GD32 link is a single serial resource the M33 already
serializes (`v2n_supervisor` mutex).  The proxy issues one in-flight op per rpc
channel (`alp_rpc_call`'s single-slot model suffices).  High-throughput callers
open multiple rpc channels — that is queue depth, not parallelism; the GD32 still
serializes.

**6. Protocol version.**  The proxy assumes the M33+GD32 are version-matched
(`PROTOCOL_VERSION 0.8.0`); the M33 owns the GD32 version handshake at boot (it
already does) and does not re-negotiate per call.  A `GET_VERSION` passthrough op
can later surface the GD32 version to the A55 if needed.

**7. Scope.**  Proxiable peripherals = those with a Zephyr `gd32_bridge`
backend: **ADC, Counter, QEnc, PWM, DAC**.  The PWM capture / single-pulse /
timer-sync ops (`0x23–0x27`) are firmware-`NOSUPPORT` today → the proxy relays
the GD32 `NOSUPPORT`.  The proxy is **V2N-M1 only** (`renesas:rzv2n:n44`); on
other Yocto SoMs the generic Linux backends (sysfs PWM, IIO ADC, …) apply.

**8. Trust.**  The A55↔M33 RPMsg link is on-SoM and trusted; the proxy schema
carries no auth/encryption — consistent with `docs/threat-model.md` (the
inter-core link is not an SDK-scope attack surface).

## Consequences

**Positive**
- The M33 dispatcher is a thin relay (one subscribe callback + the GD32
  command-layer call it already owns).  The Linux proxy backends are
  `alp_rpc_call` clients filling the existing ops vtables, registering for
  `renesas:rzv2n:n44` (winning the tiebreak over `yocto_drv`).
- No new opcode space; no schema maintained separately from the GD32 protocol —
  the proxy tracks the GD32 protocol version for free.
- All five peripherals are now proxiable (DAC reached the registry in PR #145).

**Negative / risks**
- Couples the proxy wire to the GD32 protocol version — but that coupling already
  exists (the M33 speaks GD32); the proxy just extends it over RPMsg, and a GD32
  protocol bump is already a coordinated event.
- The single-GD32-link serialization caps cross-core peripheral throughput;
  latency = RPMsg round trip + SPI transaction.  Not for high-rate streaming
  (the GD32 `ADC_STREAM` ops help, but each proxied read still pays RPMsg latency).
- **Bench-validation required:** the M33 dispatcher and the Linux proxy backends
  are both unverified until run on real V2N-M1 silicon (the `gd32g553` host driver
  is itself `[UNTESTED]`/paper-correct).

## Implementation plan (deferred — needs M33 firmware + bench)

1. **M33 dispatcher app:** `alp_rpc_open` + `alp_rpc_subscribe("gd32", cb)`; the
   callback decodes `CMD|payload` → the GD32 bridge command layer (reuse the host
   driver's command-send) → `alp_rpc_send(STATUS|reply)`.  A board config enabling
   `CONFIG_ALP_SDK_V2N_SUPERVISOR` + `CONFIG_ALP_SDK_RPC` together on the `m33_sm`
   slice (no single config does today).
2. **Linux proxy backends:** `src/backends/{adc,counter,pwm,qenc,dac}/yocto_rpmsg_proxy.c`
   — fill the ops vtable; each op builds `CMD|payload`, `alp_rpc_call`s, maps
   `STATUS→alp_status_t`.  Register `silicon_ref="renesas:rzv2n:n44"`,
   `vendor="renesas"`, priority 100.  CMake-gate behind the V2N-M1 SoM + the
   `open-amp`/`<linux/rpmsg.h>` gate (mirror `rpc/yocto_drv.c`).
3. **Tests:** a hermetic host unit test of the `CMD|payload` framing + `STATUS`
   mapping per peripheral; end-to-end is HIL.

## References

- Issue #33 (cross-core proxy section).
- `src/backends/rpc/yocto_drv.c`, `include/alp/rpc.h` (transport).
- `src/zephyr/v2n_supervisor.{c,h}`, `include/alp/chips/gd32g553.h` (M33→GD32).
- `firmware/gd32-bridge/src/protocol.{h,c}`, `docs/gd32-bridge-protocol.md` (opcode set, v0.8.0).
- `src/backends/{adc,counter,pwm,qenc,dac}/gd32_bridge.c` (the in-core reference the M33 relay reuses).
- `src/backend.c:62-104`, `include/alp/soc_caps.h` `ALP_SOC_REF_STR` (silicon_ref tiebreak).
