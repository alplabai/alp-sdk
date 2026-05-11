# vendors/nanopb

Vendor wrapper for **nanopb** -- the zlib-licensed C protobuf
encoder/decoder for embedded targets.  Upstream:
<https://github.com/nanopb/nanopb>.

**Status: SDK-internal dependency.**  Consumers do NOT enable
nanopb via `board.yaml`; it doesn't appear in the `libraries:` enum.
The SDK uses it internally for `<alp/mproc.h>` IPC frame
serialisation, and the proto schema at
`metadata/protos/alp_mproc.proto` is internal-only.  When the v0.4
mproc path lands, nanopb is pulled in unconditionally.

## Why nanopb

The Zephyr backend's mproc path (`src/zephyr/mproc_zephyr.c`) sends
raw byte blobs over `alp_mbox_send`.  That's fine for v0.2 / v0.3
but doesn't compose well as the multi-processor surface widens:

- Forward + backward compatibility between firmware versions on the
  two M55 cores (one core may upgrade ahead of the other).
- Self-describing payloads for the alp-studio runtime inspector.
- Wire-format stability for OTA scenarios where the IPC contract
  needs to survive an update.

A handwritten binary format paints itself into a corner on every
one of these.  nanopb is the canonical embedded-friendly answer --
~3 KB ROM per message type, no heap, no exceptions, zlib license
(MIT-compatible with Apache-2.0).

The proto schema lands at `metadata/protos/alp_mproc.proto` so the
v0.3 studio + v0.4 IPC firmware both code-gen against the same
source-of-truth.

## Status

**v0.4-prep.**  First consumer landed: the Zephyr `<alp/mproc.h>`
backend's IPC envelope wrapping (`CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING`).
Three pieces ship today:

- Stub `<pb.h>` + `<pb_encode.h>` + `<pb_decode.h>` here mirroring
  the upstream public ABI as of v0.4.9.  These remain pure
  scaffolding -- the placeholder framing below does NOT call into
  pb_encode / pb_decode; it hand-rolls a 12-byte LE binary header
  + payload.  The stubs let SDK source `#include <pb.h>` without
  breaking the build, ready for the swap-in when the upstream
  nanopb pack arrives.
- Source-of-truth proto schema at `metadata/protos/alp_mproc.proto`
  documenting the M55-HP <-> M55-HE message envelope, RPC request /
  response, and async notifications.
- Placeholder framing impl at `src/common/proto/alp_mproc_frame.{h,c}`
  (~140 LoC) -- the v0.4-prep stand-in for the nanopb-generated
  codec.  Same encode/decode call sites in `mproc_zephyr.c`;
  drop-in swap when the generator lands.

The placeholder's wire layout is intentionally **incompatible**
with the v0.4-final nanopb wire (a placeholder, not a draft proto
spec).  Both ends of an IPC channel must agree on the
`CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING` flag value -- mixing framed +
raw firmwares breaks the channel.

No Kconfig flag for nanopb itself (the previous
`CONFIG_ALP_SDK_USE_NANOPB` was removed in the v0.3 cleanup).
SDK-internal dependencies don't get user-visible enables; the
consumer's own Kconfig (`CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING` for
the mproc path) gates whether the library is *used*.

## Wiring (v0.4)

The west.yml pin is **shipped** as of v0.3:

```yaml
- name: nanopb
  remote: nanopb
  revision: nanopb-0.4.9    # GitHub tag format (note prefix); bump intentionally on each upgrade.
  path: modules/lib/nanopb
  groups:
    - extras-v04             # disabled by default -- see top-level group-filter
```

Behind the `extras-v04` group (disabled by default in the
manifest's `group-filter:`), so `west update` on a v0.3 workspace
does not fetch the upstream source -- the stub headers in this
directory keep SDK builds link-clean.  Flip the group on with
`west update --group-filter +extras-v04` (or edit the manifest)
when the v0.4 mproc path lands.

nanopb ships a `zephyr/module.yml` at the repo root, so Zephyr's
west import picks it up automatically once the group is enabled
-- no extra `EXTRA_ZEPHYR_MODULES` plumbing required.

The upstream module also ships `extra/nanopb.cmake` which integrates
the generator step -- v0.4 wires that against `metadata/protos/*.proto`
and emits per-message `.pb.c` + `.pb.h` pairs into the build tree.

## License

The stub headers here are **Apache-2.0** (they declare only the
public ABI of the upstream library, not its implementation).
nanopb itself is **zlib-licensed** (compatible with Apache-2.0).

## See also

- [`metadata/protos/alp_mproc.proto`](../../metadata/protos/alp_mproc.proto)
  -- the IPC frame schema this directory is the runtime for.
- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 3 "already integrated / Zephyr-native" entry.
- [`vendors/lwrb/`](../lwrb/) -- companion v0.3 scaffolding for
  the ring-buffer integration.
