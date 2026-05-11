# vendors/lwrb

Vendor wrapper for **LwRB** -- a lock-free, MIT-licensed C ring
buffer library by MaJerle.  Upstream:
<https://github.com/MaJerle/lwrb>.

**Status: SDK-internal dependency.**  Consumers do NOT enable
LwRB via `alp.yaml`; it doesn't appear in the `libraries:` enum.
The SDK uses it internally where it makes sense and pulls it in
unconditionally when the relevant subsystem is compiled.

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
  so SDK source compiles against the LwRB API even on hosts that
  haven't fetched the upstream module via west.  Once the v0.4
  audio path lands the real consumer, the west.yml pin replaces
  this stub on the include path.
- A west-manifest TODO for the upstream pin.  See "Wiring" below.

No Kconfig flag (the previous `CONFIG_ALP_SDK_USE_LWRB` was removed
in the v0.3 cleanup).  SDK-internal dependencies don't get user-
visible enables -- the audio path just uses LwRB when it's built.

## Wiring (v0.4)

LwRB does **not** ship a `zephyr/module.yml` at its repo root, so
a bare `west.yml` import won't auto-register it as a Zephyr module.
Two viable paths:

**(a) west import + `EXTRA_ZEPHYR_MODULES`** (preferred, no fork):

```yaml
# In the top-level west.yml:
remotes:
  - name: majerle
    url-base: https://github.com/MaJerle
projects:
  - name: lwrb
    remote: majerle
    revision: v3.2.0       # pin -- bump intentionally on each upgrade.
    path: modules/lib/lwrb
```

then add a tiny `modules/lib/lwrb/zephyr/module.yml` + `CMakeLists.txt`
wrapper in `alp-sdk`'s own `west.yml` self-import (or via a build-side
`EXTRA_ZEPHYR_MODULES` argument).  ~10 lines.

**(b) vendor a tagged release into `vendors/lwrb/src/`**:

If we'd rather not depend on west fetching a third party at all,
drop a tagged tarball under `vendors/lwrb/src/` and compile it
directly from our CMakeLists.  Higher carrying cost on upgrade
(manual re-vendor) but zero supply-chain surface.

v0.4 picks (a) unless we hit a reason not to.  Either way: flipping
`CONFIG_ALP_SDK_USE_LWRB=y` makes the upstream `lwrb/lwrb.h` win
the include search ahead of this directory's stub.  No source-code
change in the SDK is needed for the swap -- both headers share the
same ABI.

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
