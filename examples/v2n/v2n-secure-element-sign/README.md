# v2n-secure-element-sign

Init the OPTIGA Trust M on V2N's BRD_I2C, read its product info
object as a sanity check, and issue an **ECDSA-P256 sign** APDU
against a fixed SHA-256 message digest.

## What it shows

1. Opening BRD_I2C at 400 kHz and initialising
   [`optiga_trust_m_t`](../../../include/alp/chips/optiga_trust_m.h).
   `optiga_trust_m_init` performs the data-link-layer
   `OPEN_APPLICATION` handshake; failing this means the chip
   isn't on the bus or isn't strapped to address 0x30.
2. `optiga_trust_m_read_product_info` confirms the chip is
   responding to APDUs (not just ACKing the I2C address) and
   prints the firmware identifier / build number.
3. A hand-rolled `CalcSign` (0x31) APDU per Infineon's Solution
   Reference Manual table 16:
   ```
   Cmd=0x31 | Param=0x11(ECDSA-SHA256) | InLen(BE16)
     Tag=0x01 Len(BE16)=32 digest[32]
     Tag=0x03 Len(BE16)=2  OID[2]=0xE0 0xF0
   ```
4. `optiga_trust_m_send_apdu` clocks the APDU out, waits up to
   1 second for the reply, and prints the first 16 bytes of the
   ECDSA signature.

## Expected output (provisioned chip)

```
[se] product info: chip_type=8C8FCA0F00B2 fw_id=2624 build=0E16
[se] CalcSign reply: stacode=0x00  outlen=70  total=74
[se] signature[0..15]: 304402206A...
```

## Expected output (factory-fresh / unprovisioned chip)

```
[se] CalcSign reply: stacode=0x01  outlen=2  total=6
[se] chip reported error; check production provisioning for key slot 0xE0F0
```

`stacode=0x01` + a short payload typically means
"data object referenced does not exist" (the production line
hasn't generated an ECC key into slot 0xE0F0 yet).  See SRM
table 17 for the full status-code table.

## Wiring this into a real app

The example uses `optiga_trust_m_send_apdu` directly because the
v0.3 driver doesn't yet expose a typed `_sign(digest, out_sig)`
helper.  Once Infineon's Host Library is vendored into the SDK
and registered as a PSA driver against `<alp/security.h>`'s
MbedTLS PSA wrapper, application code can call
`alp_aead_open` / future `alp_sign_*` and get hardware
acceleration transparently -- this raw-APDU path is the
near-term bridge.

## See also

* [`<alp/chips/optiga_trust_m.h>`](../../../include/alp/chips/optiga_trust_m.h)
  -- driver header (init + product info + raw APDU send).
* Infineon "Solution Reference Manual OPTIGA Trust M"
  (`SRM_OPTIGA_Trust_M.pdf`) -- APDU command set + status codes.
