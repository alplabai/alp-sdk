# AEN-specific examples

Reference applications for the E1M-AEN family (lead part: Alif
Ensemble E8 -- dual-M55 + Ethos-U85/U55 NPUs, on-module ISP /
camera path, GPU2D).  Build any of these against an E1M-AEN SoM
populated on the E1M-EVK board; the per-example `board.yaml`
carries the exact SKU + board.

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`edgeai-vision-aen`](edgeai-vision-aen/)          | End-to-end EdgeAI vision pipeline -- CSI camera -> ISP -> Ethos-U55 inference -> OLED overlay. The flagship AEN demo. |
| [`aen-eeprom-manifest`](aen-eeprom-manifest/) | Read + decode the on-module 24C128 Alp manifest over SoC I2C2 (DesignWare, upstream i2c_dw). |
| [`aen-secure-element-sign`](aen-secure-element-sign/) | OPTIGA Trust M sanity + ECDSA-P256 sign over BRD_I2C (LPI2C0, owned by M55-HE), via the portable `<alp/...>` API. The AEN sibling of the V2N variant; the §5.2 bench OPTIGA check. |
| [`aen-se-crypto`](aen-se-crypto/) | SHA-256 known-answer + AES-128-GCM round-trip + TRNG through the portable `<alp/security.h>` surface, backed by the on-die Secure Enclave CryptoCell (the `se_cryptocell` backend rides the same MHUv2 -> SE mailbox `aen-se-service-query` proves). TRNG is live via `se_service_get_rnd_num`; SHA/AES-GCM ride the SE once the generic send seam lands, else fall through to MbedTLS-PSA. |

## Why a separate index here

The top-level [`examples/README.md`](../README.md) lists every
example.  This sub-index exists because AEN-specific examples
need an AEN-family SoM (no fallback path on V2N / N93) -- having
them in their own directory makes that constraint visible from
the filesystem layout alone.  Cross-family examples (gpio,
i2c, pwm, ...) stay at the top level of `examples/`.

## See also

- [`docs/soms/aen.md`](../../docs/soms/aen.md) -- AEN SoM
  one-pager + supported peripherals.
- [`docs/getting-started.md`](../../docs/getting-started.md) --
  EVK bring-up flow that lands you ready to run these examples.
- AEN feature coverage matrix -- maintained in the
  internal `alp-sdk-internal` repo as `AEN-FEATURE-AUDIT-2026-05.md`
  (what the SDK exposes vs the silicon's full surface).
