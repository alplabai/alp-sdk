# aen-secure-element-sign

Probe the OPTIGA Trust M on the E1M-AEN's **BRD_I2C** and confirm the
current probe-only driver contract.  The driver reads I2C_STATE to prove
the part is reachable; product-info and raw-APDU helpers return
`ALP_ERR_NOSUPPORT` until the Infineon host-library transport is
integrated.

This is the **E1M-AEN (Alif Ensemble) sibling** of
[`examples/v2n/v2n-secure-element-sign`](../../v2n/v2n-secure-element-sign).
The `src/` is intentionally parallel -- everything goes through the SoM-portable
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
   `optiga_trust_m_init` performs an I2C_STATE register read only;
   failing this means the chip is not on the bus or is not strapped to
   address 0x30.
2. `optiga_trust_m_read_product_info` returns `ALP_ERR_NOSUPPORT`
   because GET_DATA_OBJECT needs the full APDU transport.
3. `optiga_trust_m_send_apdu` validates a non-empty APDU buffer and
   returns `ALP_ERR_NOSUPPORT` without fabricating a signature.

## Expected output (OPTIGA-populated SoM)

```
[se] I2C_STATE probe -> ALP_OK
[se] read_product_info -> -5 (expected NOSUPPORT)
[se] send_apdu -> -5 resp_len=0 (expected NOSUPPORT, zero bytes)
[se] RESULT PASS: Trust M I2C_STATE probe works; product-info/raw-APDU are cleanly blocked with ALP_ERR_NOSUPPORT
```

## Expected output (current AEN bench gates)

```
[se] RESULT SKIP: alp_i2c_open failed: -2 (BRD_I2C/LPI2C0 not ready on this bench)
```

or, once BRD_I2C opens but the assembly is OPTIGA-DNI:

```
[se] RESULT SKIP: optiga_trust_m_init -> -2 (Trust M not ACKing; current AEN bench assemblies may be OPTIGA-DNI)
```

The eventual signing path belongs with the Infineon host-library/PSA
integration, not a partial hand-rolled APDU transport in this example.

## See also

* [`examples/v2n/v2n-secure-element-sign`](../../v2n/v2n-secure-element-sign)
  -- the V2N variant with the same probe-only contract.
* [`<alp/chips/optiga_trust_m.h>`](../../../include/alp/chips/optiga_trust_m.h)
  -- driver header (I2C_STATE probe + NOSUPPORT APDU/product-info stubs).
* [`docs/bring-up-aen.md`](../../../docs/bring-up-aen.md) §5.2 -- where
  this example is the OPTIGA bench sanity check.
* Infineon "Solution Reference Manual OPTIGA Trust M"
  (`SRM_OPTIGA_Trust_M.pdf`) -- APDU command set + status codes.
