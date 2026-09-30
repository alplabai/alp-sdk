# 0035. micro-ROS cross-core transport: XRCE custom transport over the ADR 0016 RPMsg channel

Status: Proposed
Date: 2026-09-30
Deciders: maintainer (this document recommends; it does not decide)
Relates to: [0010](0010-heterogeneous-os-orchestration.md),
[0016](0016-cross-core-peripheral-proxy-wire-schema.md),
[0018](0018-curated-third-party-libraries.md)

Tracks issue #373 (cross-core RMW bridge, "needs ADR + bench"); unblocks the
teaching example in #375. **The maintainer decides.** Nothing here is measured:
every number or behaviour marked *to verify* is a bench or upstream-source
question, and the recommendation stands only if the bench evidence in "What
confirms it" comes back as predicted.

## Context

ADR 0018's flagship is a micro-ROS node on the Cortex-M33 (Zephyr) exchanging
topics with ROS 2 on the Cortex-A55 (Yocto). What exists today:

- `micro_ros_zephyr_module` is pinned in `west.yml` (Humble branch, revision
  `cfbddc5e4334317a1036e883ce8f6af12b1da66a`, #370/#371), enabled by
  `CONFIG_MICROROS=y`. `metadata/libraries/micro-ros.yaml` is Tier B.
- ROS 2 Humble on the A side is grounded in `meta-alp-sdk`
  (`recipes-ros/alp-perception`). No micro-ROS **agent** is packaged; #372 is
  the live blocker for the image path.
- The A55<->M33 channel: `<alp/rpc.h>` (`alp_rpc_call` / `alp_rpc_send`,
  payload `method\0bytes`, opaque to the SDK), the ADR 0016 wire contract,
  `examples/multicore/rpmsg-v2n`, and on V2N the A55 backend
  `src/backends/rpc/yocto_uio_drv.c` (priority 150, `renesas:rzv2n:n44`):
  userspace open-amp/libmetal attached to UIO regions, **not** the kernel
  `/dev/rpmsg*` chardev, because the RZ/V2N Yocto BSP does not carry the
  mainline `rpmsg_char` glue. The UIO nodes and the A55 reserved-memory window
  are not shipped in `meta-alp-sdk` yet (#2374, open).

Two facts reshape the issue's framing:

1. **No custom RMW is needed.** micro-ROS's RMW is `rmw_microxrcedds`, a client
   of Micro XRCE-DDS. The pluggable seam is the XRCE *transport*
   (`rmw_uros_set_custom_transport` on the client; a custom transport on the
   agent side). "Custom RMW over RPMsg" is really "custom transport over
   RPMsg". *To verify against the pinned Humble module.*
2. **The V2N kernel has no rpmsg netdev or tty**, since the BSP lacks the
   kernel rpmsg stack. Anything needing a kernel netdev or tty backed by RPMsg
   is off the table without a kernel change, which is a separate, larger
   decision.

Requirement (issue #373 and its triage comments): a micro-ROS node on the CM33
publishes and subscribes to ROS 2 topics with the agent on the A55, on real
RZ/V2N. V2N first; AEN/A32 is scoped separately.

## Options

### A. UDP over a virtual network (virtio-net / rpmsg-net)

micro-ROS's stock UDP transport; the agent listens on the virtual link's IP.

- Latency: IP+UDP framing and a network-stack traversal on both sides.
- CM33 footprint: largest. Needs a Zephyr IP stack plus a virtual netdev
  driver over the shared memory; availability with the pinned Zephyr v4.4.1
  and this open-amp is *to verify*. Costs RAM the CM33 budget may not have.
- Reuse of the OpenAMP UIO path (#2374): none directly. The A55 needs a
  kernel netdev over the same memory, which the UIO-only BSP does not provide.
- Agent placement: stock, unmodified agent on the A55.
- Security/ownership: a network-visible socket on an interface the SDK does
  not own; widens the surface that ADR 0016 clause 8 treats as trusted
  on-SoM.

### B. Custom XRCE transport over the ADR 0016 RPMsg channel (recommended)

The CM33 registers `rmw_uros_set_custom_transport` callbacks (open/close/
write/read) backed by an open-amp endpoint. A small A55 process,
`alp-microros-bridge`, uses `alp_rpc_*` (so `yocto_uio_drv.c` on V2N,
unchanged) to carry XRCE datagrams and hands them to an **unmodified**
micro-ROS agent over UDP on `127.0.0.1`.

- Latency: one RPMsg hop plus one loopback hop on the A55; adds a userspace
  copy on the A55. Absolute figures are *to verify*.
- CM33 footprint: no IP stack; a shim of a few callbacks plus the open-amp
  endpoint the rpmsg-v2n example already links. Packet-oriented: no XRCE
  serial framing or CRC, since RPMsg already frames messages.
- Reuse of the UIO path: full. It is the ADR 0016 substrate with one new
  method name (e.g. `"xrce"`) and the XRCE datagram as payload. Inherits the
  #2374 prerequisite.
- Agent placement: stock agent, stock UDP transport; only the bridge is ours.
- Security/ownership: no new network exposure; the bridge owns the channel and
  the agent sees loopback only. The link stays on-SoM and trusted (ADR 0016
  clause 8).
- Cost: we own one small bridge and its MTU handling. The XRCE custom-transport
  MTU (`UXR_CONFIG_CUSTOM_TRANSPORT_MTU`, *to verify*) must fit the RPMsg
  buffer payload, which comes from the M33 resource table (*to verify* the
  actual size).

### C. XRCE serial transport over RPMsg (pty bridge)

As B, but the bridge exposes a pty and the agent runs its stock `serial`
transport; the CM33 uses XRCE serial framing (HDLC-style + CRC-16).

- Latency: B plus framing/CRC CPU on both ends and a pty hop on the A55.
- CM33 footprint: B plus the framing code (small).
- Reuse of the UIO path: same as B.
- Agent placement: stock, but a byte stream layered on a message channel:
  redundant framing and CRC over RPMsg, and resync logic the message channel
  makes unnecessary.
- Security/ownership: as B.
- Only real advantage over B: no loopback UDP socket on the A55.

## Recommendation

**Option B**, subject to bench confirmation. It reuses the ADR 0016 channel and
`yocto_uio_drv.c` as-is, keeps the CM33 free of an IP stack, keeps the agent
unmodified, adds no network surface, and avoids C's redundant framing. A is
disqualified on V2N by the missing kernel rpmsg netdev (and by CM33 footprint);
revisit only if a future BSP ships kernel rpmsg networking. C is the fallback
if B's loopback hop is rejected on review.

## What confirms it (bench, RZ/V2N, serial, outside any workflow)

Prerequisite: #2374 lands (or the vendor overlay is applied) so the UIO nodes
and reserved window exist, and `examples/multicore/rpmsg-v2n` round-trips.

1. A CM33 micro-ROS node publishes `std_msgs/Int32` at a fixed rate and
   `ros2 topic echo` on the A55 (agent + bridge) sees every sample; the reverse
   direction (A55 publisher, CM33 subscriber) works.
2. Loss and ordering over a sustained run (duration and rate set on the bench);
   recovery after restarting the agent and after a CM33 reset (XRCE session
   re-creation).
3. Round-trip latency distribution (median and tail) against the bare
   `alp_rpc_call` baseline from `rpmsg-v2n`, to show what bridge + XRCE add.
   B is falsified if the added tail latency or loss is unacceptable for the
   target topic rates (the maintainer sets the threshold).
4. CM33 RAM/flash delta of `CONFIG_MICROROS=y` plus the shim, from the map
   file, against the CM33 budget. This falsifies every option if micro-ROS
   itself does not fit.
5. RPMsg buffer payload size read from the resource table, compared with the
   XRCE MTU actually configured.

Evidence must come from a real run, not simulation.

## Minimal implementation plan (no code in this ADR)

1. West pin: **done** (`micro_ros_zephyr_module` at `cfbddc5e`, #370/#371).
2. #2374: `meta-alp-sdk` DT reserved-memory + UIO nodes, gated, so the V2N A55
   has a peer.
3. CM33: a transport shim behind `CONFIG_MICROROS` registering the
   custom-transport callbacks over the open-amp endpoint; extend
   `examples/multicore/rpmsg-v2n` or add a sibling under `examples/multicore/`
   rather than a new channel.
4. A55: `meta-alp-sdk` recipes for `micro_ros_agent` (Humble, alongside the
   `meta-ros2-humble` layer, #372) and for `alp-microros-bridge`, behind an
   opt-in image feature, not the default image.
5. Bench per the list above; then promote `micro-ros` Tier B to A (the Tier A bar in `metadata/libraries/README.md`:
   CI lane, shipped example, breakage blocks release) and deliver the teaching
   example (#375).

## Consequences

Good: no RMW written or forked; no agent fork; no IP stack on the CM33; no new
wire schema (one method name over ADR 0016).

Bad / costs: we own the A55 bridge; depends on #2374 and #372; the loopback hop
and userspace copy add latency; ADR 0016 is itself still Proposed, so this ADR
inherits that uncertainty; AEN/A32 is out of scope and needs its own decision.

## Open questions for the maintainer

- Accept B, or prefer C to avoid the loopback socket?
- Should the bridge live in `meta-alp-sdk` as its own recipe or ship with the
  agent recipe?
- Latency and loss acceptance thresholds for bench item 3.
