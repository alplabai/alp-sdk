# v2n-secure-element-sign

Probe the OPTIGA Trust M on V2N's BRD_I2C and confirm the current
probe-only driver contract.  The driver reads I2C_STATE to prove the
part is reachable; product-info and raw-APDU helpers return
`ALP_ERR_NOSUPPORT` until the Infineon host-library transport is
integrated.

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

## Expected output (missing or held-in-reset chip)

```
[se] RESULT FAIL: optiga_trust_m_init -> -2 (Trust M not ACKing)
```

The eventual signing path belongs with the Infineon host-library/PSA
integration, not a partial hand-rolled APDU transport in this example.

## Wiring this into a real app

The example calls `optiga_trust_m_send_apdu` only to prove the current
driver returns `ALP_ERR_NOSUPPORT` for APDU transport.  Once Infineon's
Host Library is integrated and registered as a PSA driver against
`<alp/security.h>`'s MbedTLS PSA wrapper, application code can call
`alp_aead_open` / future `alp_sign_*` and get hardware acceleration
transparently.

## See also

* [`<alp/chips/optiga_trust_m.h>`](../../../include/alp/chips/optiga_trust_m.h)
  -- driver header (I2C_STATE probe + NOSUPPORT APDU/product-info stubs).
* Infineon "Solution Reference Manual OPTIGA Trust M"
  (`SRM_OPTIGA_Trust_M.pdf`) -- APDU command set + status codes.
