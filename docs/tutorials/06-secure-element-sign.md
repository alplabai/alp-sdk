<!-- Last verified: 2026-05-18 against slice-3b state. -->

# 06 -- Secure-element probe contract

Walks `examples/v2n/v2n-secure-element-sign/` -- initialise the
Infineon OPTIGA Trust M probe context, read I2C_STATE, and confirm the
current product-info/raw-APDU entry points return `ALP_ERR_NOSUPPORT`
until the Infineon host-library transport is integrated.

## When to read this

* You're building a device that needs hardware-rooted identity
  (eventually a TLS client cert via a secure-element-stored private key).
* You're integrating an OEM provisioning flow and need to separate
  "chip reachable on I2C" from "APDU/product-info transport implemented".
* You want to understand why the SDK exposes planned product-info and
  raw-APDU functions that deliberately return `ALP_ERR_NOSUPPORT` today.

## Wire shape

OPTIGA Trust M sits on BRD_I2C at 7-bit `0x30`.  Its host
interface is a layered stack: data-link layer (frames + CRC16) +
application protocol (PRESET/GET frames) + APDU command set.

The SDK's current `optiga_trust_m_init` does not wrap that lower
transport and does not send `OPEN_APPLICATION`.  It reads the
I2C_STATE register to prove the chip ACKs on the bus.  The planned
`optiga_trust_m_read_product_info` and `optiga_trust_m_send_apdu`
entry points validate their arguments, then return `ALP_ERR_NOSUPPORT`
because GET_DATA_OBJECT and CalcSign both need the full info-pack/APDU
stack.

## Future signing path

Key slot `0xE0F0` remains OPTIGA's canonical "device endpoint"
identity slot, and production-test should provision an ECC private key
there.  The SDK should wire that through Infineon's Host Library and a
PSA/MbedTLS backend rather than a hand-rolled APDU transport in this
example.  When that lands, application code should use the portable
security API instead of direct raw APDUs.

## Sample failure modes the example exercises

| Symptom                          | Cause                                                |
|----------------------------------|------------------------------------------------------|
| `optiga_trust_m_init -> -2`      | Chip not present / mis-strapped on this board.       |
| `read_product_info -> ALP_ERR_NOSUPPORT` | Current driver does not ship APDU transport. |
| `send_apdu -> ALP_ERR_NOSUPPORT` | Current driver does not ship APDU transport. |
| `RESULT PASS`                    | I2C_STATE probe works and APDU/product-info are blocked honestly. |

## See also

* [`examples/v2n/v2n-secure-element-sign/`](../../examples/v2n/v2n-secure-element-sign/)
* [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h)
* Infineon "Solution Reference Manual OPTIGA Trust M" (vendor doc).
