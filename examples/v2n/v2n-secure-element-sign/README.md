# v2n-secure-element-sign

Talk to the OPTIGA Trust M on V2N's BRD_I2C from a Linux/Yocto
user-space app on the V2N Cortex-A55 cluster: probe it, read its
Coprocessor UID, and run a raw APDU session.  The driver runs
Infineon's host library (`vendors/optiga-trust-m`) through a PAL on the
portable `<alp/peripheral.h>` I2C and timing calls.  Nothing here
writes to the chip.

> RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
> (`metadata/e1m_modules/v2n/core-ownership.yaml`) -- the CM33 must
> never master it. This app runs on the A55, following the same
> pattern as [`v2n-power-monitor`](../v2n-power-monitor/) (portable
> `<alp/i2c.h>` + a natural-name chip driver, Linux `/dev/i2c-N`
> backend).

## What it shows

1. Opening BRD_I2C (`bus_id = 8`, Linux `/dev/i2c-8`; meta-alp-sdk's
   `e1m-v2n-som.dtsi` aliases `i2c8 = &i2c8;`) at 400 kHz and
   initialising
   [`optiga_trust_m_t`](../../../include/alp/chips/optiga_trust_m.h).
   `optiga_trust_m_init_with_reset` reads the I2C_STATE register only.
   A Trust M idle for more than about 10 s can stop ACKing until it is
   hardware-reset (#2507), so the app opens the GD32 bridge and passes
   `gd32g553_se_reset_hook` as the reset hook: if the part is silent the
   driver pulses SE_RST once and probes again.  Failing after that means
   the chip is not on the bus, is not strapped to address 0x30, or is held
   in reset.  With no reachable GD32 the app passes no hook (plain probe).
2. `optiga_trust_m_read_product_info` opens the Trust M application
   and reads the 27-byte Coprocessor UID (data object 0xE0C2).
3. `optiga_trust_m_send_apdu` runs a raw session: the app sends
   OpenApplication itself, then GetDataObject(0xE0C2), and checks the
   bytes match step 2.

## Expected output (OPTIGA-populated SoM)

Bench, E1M-V2M103 2026W38-0001 on the E1M-X EVK V2:

```
[se] I2C_STATE probe -> ALP_OK
[se] UID: cim CD platform 16 model 33 fw 80101071 build 2564
[se] raw APDU UID matches (27 bytes)
[se] RESULT PASS: Trust M probe, Coprocessor UID and raw APDU session work
```

## Expected output (missing or held-in-reset chip)

```
[se] RESULT FAIL: optiga_trust_m_init (Trust M not ACKing) -> -2
```

## Wiring this into a real app

The raw APDU session reaches every Trust M command (key generation,
ECDSA, secure NVM) today, with the caller owning the command bytes and
the application session.  Typed calls for those commands, and a PSA
driver registered with `<alp/security.h>`'s MbedTLS wrapper so
`alp_aead_open` / future `alp_sign_*` use the chip transparently, are
still to come.

## See also

* [`<alp/chips/optiga_trust_m.h>`](../../../include/alp/chips/optiga_trust_m.h)
  -- driver header.
* `vendors/optiga-trust-m/README.md` -- the vendored host library and its
  alp PAL.
* Infineon "Solution Reference Manual OPTIGA Trust M"
  (`SRM_OPTIGA_Trust_M.pdf`) -- APDU command set + status codes.
