# vendors/lwrb

Vendor wrapper for **LwRB** -- a lock-free, MIT-licensed C ring
buffer library by MaJerle.  Upstream:
<https://github.com/MaJerle/lwrb>.

## Why LwRB

The Zephyr backend's audio path (`src/zephyr/audio_zephyr.c`) uses
Zephyr's `k_mem_slab` today, which is fine for fixed-block DMA
staging.  But several v0.4 sites want a true byte-granular ring
with `producer / consumer in different ISRs`-safe semantics:

- UART RX scratch in `src/zephyr/peripheral_uart.c` (poll-mode
  reads draining a producer-side ISR).
- The `alp_audio_in_*` path on V2N once the I²S RX hand-off shifts
  off the slab-block model.
- The `alp_mbox_*` notification queue on M55-HP <-> M55-HE in
  the v0.3 multi-proc completion.

Hand-rolling a lock-free ring per use site is error-prone (the
classic empty/full disambiguation bug is easy to retrocomplete);
LwRB has shipped the canonical implementation under MIT for years.
~300 LoC + a small `lwrb_ex.c` for stream-block helpers.

License + maintenance + footprint all pass the
`docs/recommended-libraries.md` evaluation principles.

## Status

**v0.3 scaffolding.**  Lands the integration anchor:

- A stub `<lwrb/lwrb.h>` here that mirrors the upstream API surface
  so SDK source compiles against `CONFIG_ALP_SDK_USE_LWRB=y` even
  on hosts that haven't fetched the upstream module via west.
- A `CONFIG_ALP_SDK_USE_LWRB` Kconfig flag at `zephyr/Kconfig`,
  default OFF.  When ON, audio + UART backends are free to call
  `lwrb_*` -- v0.3 doesn't switch any sites yet; v0.4 lands the
  first real consumer.
- A west-manifest TODO for the upstream pin.  See "Wiring" below.

## Wiring (v0.4)

Add to the top-level `west.yml` projects list:

```yaml
- name: lwrb
  remote: majerle
  revision: v3.2.0       # pin -- bump intentionally on each upgrade.
  path: modules/lib/lwrb
```

And add the matching `majerle` remote:

```yaml
- name: majerle
  url-base: https://github.com/MaJerle
```

When that's in place, flipping `CONFIG_ALP_SDK_USE_LWRB=y` makes
the upstream `lwrb/lwrb.h` win the include search ahead of this
directory's stub.  No source-code change in the SDK is needed for
the swap -- both headers share the same ABI.

## License

The stub header here is **Apache-2.0** (it declares only the public
ABI of the upstream library, not its implementation).  LwRB itself
is **MIT-licensed** (compatible with Apache-2.0 for the SDK's
license terms).

## See also

- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 3 "already integrated / Zephyr-native" entry.
- [`vendors/nanopb/`](../nanopb/) -- companion v0.3 scaffolding for
  protobuf-based IPC framing.
