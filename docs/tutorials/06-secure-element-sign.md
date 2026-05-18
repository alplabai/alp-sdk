<!-- Last verified: 2026-05-18 against slice-3b state. -->

# 06 -- Secure-element ECDSA sign

Walks `examples/v2n/v2n-secure-element-sign/` -- initialise the
Infineon OPTIGA Trust M, read its product-info object, then issue
a hand-rolled `CalcSign` APDU to ECDSA-sign a fixed SHA-256
digest.

## When to read this

* You're building a device that needs hardware-rooted identity
  (TLS client cert via a secure-element-stored private key).
* You're integrating an OEM provisioning flow and need to verify
  the OPTIGA's product-info matches what production-test stamped.
* You want to understand the "raw APDU" surface the SDK exposes
  before the typed PSA-driver bridge lands.

## Wire shape

OPTIGA Trust M sits on BRD_I2C at 7-bit `0x30`.  Its host
interface is a layered stack: data-link layer (frames + CRC16) +
application protocol (PRESET/GET frames) + APDU command set.

The SDK's `optiga_trust_m_send_apdu` wraps the lower two layers
and lets the caller hand it a raw APDU body.  Useful for any
command Infineon documents in their SRM but the SDK hasn't typed
yet (the typed PSA bridge lands when Infineon's host library
gets vendored).

## CalcSign APDU layout

```
Cmd=0x31 | Param=0x11 (ECDSA-SHA256) | InLen(BE16)
   Tag=0x01 Len(BE16)=32 digest[32]      -- the digest to sign
   Tag=0x03 Len(BE16)=2  OID[2]=0xE0 0xF0 -- key slot identifier
```

Key slot `0xE0F0` is OPTIGA's canonical "device endpoint" identity
slot.  Production-test provisions an ECC private key into it; if
the unit hasn't been provisioned, the chip replies with status
`0x01 0x02` ("data object referenced does not exist").

## Response shape

```
resp[0]    = StaCode  (0x00 = OK)
resp[1]    = UndefByte
resp[2..3] = OutLen (BE16; signature byte count)
resp[4..]  = ASN.1 DER signature   (typically 70..72 bytes for P-256)
```

## Sample failure modes the example exercises

| Symptom                          | Cause                                                |
|----------------------------------|------------------------------------------------------|
| `optiga_trust_m_init -> -2`      | Chip not present / mis-strapped on this board.       |
| `read_product_info -> ALP_ERR_NOSUPPORT` | v0.3 driver doesn't ship the typed APDU yet. |
| `stacode = 0x01, outlen = 2`     | Key slot 0xE0F0 not provisioned (factory-fresh chip).|
| `stacode = 0x00, outlen ≈ 70`    | Success.  First 16 hex bytes of signature printed.   |

## See also

* [`examples/v2n/v2n-secure-element-sign/`](../../examples/v2n/v2n-secure-element-sign/)
* [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h)
* Infineon "Solution Reference Manual OPTIGA Trust M" (vendor doc).
