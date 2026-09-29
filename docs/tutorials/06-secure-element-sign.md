<!-- Last verified: 2026-09-29 on E1M-V2M103 2026W38-0001 (E1M-X EVK V2). -->

# 06 -- Talking to the secure element

Walks `examples/v2n/v2n-secure-element-sign/` -- probe the Infineon
OPTIGA Trust M, read its Coprocessor UID, and run a raw APDU session.
Nothing in the example writes to the chip.

## When to read this

* You're building a device that needs hardware-rooted identity
  (eventually a TLS client cert via a secure-element-stored private key).
* You're integrating an OEM provisioning flow and need to separate
  "chip reachable on I2C" from "chip answering commands".
* You need a Trust M command the driver has no typed call for yet.

## Wire shape

OPTIGA Trust M sits on BRD_I2C at 7-bit `0x30`.  Its host interface is
a layered stack: an I2C physical layer (register reads and writes), a
data-link layer (sequence-numbered frames + CRC16), a transport layer
(APDUs chained across frames), then the APDU command set.

The SDK does not reimplement that stack.  `chips/optiga_trust_m` runs
Infineon's host library (`vendors/optiga-trust-m`) through a PAL built
on `alp_i2c_*` and `alp_uptime_ms`, so the same driver runs on the A55
and on an MCU core.

* `optiga_trust_m_init` reads the I2C_STATE register only: the cheapest
  "is it fitted" check.  It rides out the part's wake-from-sleep NACKs.
* `optiga_trust_m_read_product_info` opens the Trust M application and
  reads the Coprocessor UID (data object `0xE0C2`).
* `optiga_trust_m_send_apdu` runs a raw session on the library's comms
  layer.  The first call opens a fresh link, so the caller sends
  OpenApplication (`F0 00 00 10` + the 16-byte application ID) before
  any other command.

## Signing path

Key slot `0xE0F0` is OPTIGA's canonical "device endpoint" identity slot,
and production test should provision an ECC private key there.  Until
typed calls and a PSA/MbedTLS driver land, a signature is reachable
through the raw APDU session (CalcSign), with the caller building the
command.  Generating or writing a key is a write to the chip: plan it as
a provisioning step, not an example run.

## Sample failure modes the example exercises

| Symptom                                   | Cause                                              |
|-------------------------------------------|----------------------------------------------------|
| `optiga_trust_m_init ... -> -2`           | Chip not present / mis-strapped on this board.     |
| `read_product_info -> -4` (TIMEOUT)       | Chip stopped answering mid-session.                |
| `raw OpenApplication` / `GetDataObject` fail | A command APDU was refused; the status byte is printed. |
| `RESULT PASS`                             | Probe, UID read and raw session all worked.        |

## See also

* [`examples/v2n/v2n-secure-element-sign/`](../../examples/v2n/v2n-secure-element-sign/)
* [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h)
* [`vendors/optiga-trust-m/`](../../vendors/optiga-trust-m/README.md)
* Infineon "Solution Reference Manual OPTIGA Trust M" (vendor doc).
