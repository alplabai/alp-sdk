# aen-secure-element-sign

Init the OPTIGA Trust M on the E1M-AEN's **BRD_I2C**, read its product
info object as a sanity check, and issue an **ECDSA-P256 sign** APDU
against a fixed SHA-256 message digest.

This is the **E1M-AEN (Alif Ensemble) sibling** of
[`examples/v2n/v2n-secure-element-sign`](../../v2n/v2n-secure-element-sign).
The `src/` is identical — everything goes through the SoM-portable
`<alp/*>` API — so the only AEN-specific facts are:

- **BRD_I2C is the Alif LPI2C0** (the LP-island I2C, `P7_4 SCL_A` /
  `P7_5 SDA_A`), surfaced as portable bus 0, carrying the Trust M at
  `0x30` alongside the RTC + EEPROM + TMP112.
- BRD_I2C lives in the low-power domain, so it is owned by the
  **M55-HE** subsystem — hence `board.yaml`'s app core is `m55_he`
  and the board target is `…/rtss_he`.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-secure-element-sign
west flash
```

> **Bench note:** running on hardware needs the AEN board's BRD_I2C
> (LPI2C0) enabled + mapped to portable bus 0 (the alp-sdk Alif LPI2C
> driver bring-up). Until that lands, the example builds (CI builds it
> under `native_sim`) but `alp_i2c_open(bus_id=0)` will not find the
> chip on a board that hasn't wired LPI2C0 yet.
>
> **The current E1M-AEN801 bench batch does not populate the OPTIGA**
> (DNI), so this example has nothing to talk to on those boards — it is
> for OPTIGA-populated SoMs. OPTIGA is part of the E1M-AEN801 SoM design
> (`on_module`); the absence is a current-batch population fact.

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

## See also

* [`examples/v2n/v2n-secure-element-sign`](../../v2n/v2n-secure-element-sign)
  -- the V2N variant (identical `src/`).
* [`<alp/chips/optiga_trust_m.h>`](../../../include/alp/chips/optiga_trust_m.h)
  -- driver header (init + product info + raw APDU send).
* [`docs/bring-up-aen.md`](../../../docs/bring-up-aen.md) §5.2 -- where
  this example is the OPTIGA bench sanity check.
* Infineon "Solution Reference Manual OPTIGA Trust M"
  (`SRM_OPTIGA_Trust_M.pdf`) -- APDU command set + status codes.
