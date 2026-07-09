# vendors/lwrb

Vendor wrapper for **LwRB** -- a lock-free, MIT-licensed C ring
buffer library by MaJerle.  Upstream:
<https://github.com/MaJerle/lwrb>.

**Status: SDK-internal dependency.**  Consumers do NOT enable
LwRB via `board.yaml`; it doesn't appear in the `libraries:` enum.
The SDK uses it internally where it makes sense and pulls it in
unconditionally when the relevant subsystem is compiled.

## Why LwRB

The Zephyr backend's audio path (`src/zephyr/audio_zephyr.c`) uses
Zephyr's `k_mem_slab` today, which is fine for fixed-block DMA
staging.  But several sites want a true byte-granular ring with
`producer / consumer in different ISRs`-safe semantics:

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

**Interim / deferred as of v0.9.**  There is no committed release
that flips the upstream module on -- the in-tree stub is the real,
shipping implementation today, not a placeholder waiting on a
specific tag.  First (and so far only) consumer landed: the Zephyr
UART backend's opt-in byte-granular RX ring buffer
(`alp_uart_rx_ringbuf_*`, gated on `CONFIG_ALP_SDK_UART_RX_RINGBUF`).
The vendor anchor ships:

- The stub `<lwrb/lwrb.h>` (Apache-2.0) declaring the upstream
  public ABI as of v3.2.0.
- An in-tree stub impl at `vendors/lwrb/src/lwrb_stub_impl.c`
  that fills in the non-inline `lwrb_write` / `lwrb_read` /
  `lwrb_peek` / `lwrb_get_full` / `lwrb_get_free` / `lwrb_skip` /
  `lwrb_advance` symbols.  Correct single-producer /
  single-consumer semantics with the canonical empty/full
  disambiguation; ~140 LoC.  Replaced wholesale by upstream
  `MaJerle/lwrb` once the `extras-lwrb-nanopb` west group is
  enabled.
- The west-manifest pin (`MaJerle/lwrb@v3.2.0`) sitting behind
  the `extras-lwrb-nanopb` group filter, ready to flip on whenever
  the Zephyr-module wiring below lands -- no version is currently
  committed to that work.

The Zephyr build picks up the vendor headers via
`zephyr/CMakeLists.txt`'s `zephyr_include_directories(...lwrb/include)`
and compiles the stub impl only when no upstream LwRB module is
on the workspace (`if(NOT DEFINED ZEPHYR_LWRB_MODULE_DIR)`).  The
plain-CMake / Yocto build always compiles the stub impl --
upstream LwRB isn't on the Yocto sysroot.

No Kconfig flag for LwRB itself (the previous
`CONFIG_ALP_SDK_USE_LWRB` was removed in the v0.3 cleanup).
SDK-internal dependencies don't get user-visible enables -- the
consumer's own Kconfig flag (`CONFIG_ALP_SDK_UART_RX_RINGBUF` for
the UART path) gates whether the library is *used*; the library
itself is pulled in unconditionally.

## Wiring (deferred, no committed release)

The west.yml pin is **shipped** (pinned since v0.3):

```yaml
- name: lwrb
  remote: majerle
  revision: v3.2.0
  path: modules/lib/lwrb
  groups:
    - extras-lwrb-nanopb   # disabled by default -- see top-level group-filter
```

Behind the `extras-lwrb-nanopb` group (disabled by default in the
manifest's `group-filter:`), so a default `west update` does not
fetch the upstream source -- the stub header in this directory
keeps SDK builds link-clean.  Flip the group on with
`west update --group-filter +extras-lwrb-nanopb` (or edit the
manifest) when the Zephyr-module wiring below lands.

LwRB does **not** ship a `zephyr/module.yml` at its repo root, so
a bare `west.yml` import won't auto-register it as a Zephyr module
even with the group on.  Two viable paths once the group is
enabled:

**(a) west import + `EXTRA_ZEPHYR_MODULES`** (preferred, no fork):

Add a tiny `modules/lib/lwrb/zephyr/module.yml` + `CMakeLists.txt`
wrapper in `alp-sdk`'s own `west.yml` self-import (or via a
build-side `EXTRA_ZEPHYR_MODULES` argument).  ~10 lines.

**(b) vendor a tagged release into `vendors/lwrb/src/`**:

If we'd rather not depend on west fetching a third party at all,
drop a tagged tarball under `vendors/lwrb/src/` and compile it
directly from our CMakeLists.  Higher carrying cost on upgrade
(manual re-vendor) but zero supply-chain surface.

The upstream swap picks (a) unless we hit a reason not to.  Either way: once
the upstream sources are on the include path the real
`lwrb/lwrb.h` wins ahead of this directory's stub.  No
source-code change in the SDK is needed for the swap -- both
headers share the same ABI.

## License

The stub header here is **Apache-2.0** (it declares only the public
ABI of the upstream library, not its implementation).  LwRB itself
is **MIT-licensed** (compatible with Apache-2.0 for the SDK's
license terms).

## See also

- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 3 "already integrated / Zephyr-native" entry.
- [`vendors/nanopb/`](../nanopb/) -- companion SDK-internal
  scaffolding for protobuf-based IPC framing (same
  interim/deferred status).
