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

**v0.3 scaffolding.**  Two pieces ship:

- A stub `<pb.h>` + `<pb_encode.h>` + `<pb_decode.h>` here that
  mirrors the upstream public ABI; SDK source compiles against the
  nanopb API even when the upstream library isn't on the include
  path.  When the v0.4 mproc path lands, the west.yml pin replaces
  this stub.
- The first protocol schema at `metadata/protos/alp_mproc.proto`
  documenting the M55-HP <-> M55-HE message envelope, RPC request /
  response, and async notifications.

No Kconfig flag (the previous `CONFIG_ALP_SDK_USE_NANOPB` was
removed in the v0.3 cleanup).  SDK-internal dependencies don't get
user-visible enables -- the mproc path just uses nanopb when it's
built.

## Wiring (v0.4)

Add to the top-level `west.yml` projects list:

```yaml
- name: nanopb
  remote: nanopb
  revision: nanopb-0.4.9   # GitHub tag format (note prefix); bump intentionally on each upgrade.
  path: modules/lib/nanopb
```

And add the matching `nanopb` remote:

```yaml
- name: nanopb
  url-base: https://github.com/nanopb
```

(nanopb ships a `zephyr/module.yml` at the repo root, so Zephyr's
west import picks it up automatically once the pin lands -- no
extra `EXTRA_ZEPHYR_MODULES` plumbing required.)

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
