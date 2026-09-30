# microros-ros2-v2n

> **Status: build-ready, not yet built or run on hardware.** The A55
> bridge compiles warning-free on a Linux host; the M33 firmware needs
> `micro_ros_zephyr_module`'s colcon cross-build, which has not been run
> for it. See [What is untested](#what-is-untested). Nothing here is
> bench evidence.

A **micro-ROS node on the RZ/V2N Cortex-M33** (Zephyr) publishes a
`std_msgs/Int32` counter that **ROS 2 on the Cortex-A55** (Yocto) can
`ros2 topic echo`. This is the teaching example for the ADR 0018
flagship, built as
[ADR 0035](../../../docs/adr/0035-micro-ros-cross-core-transport.md)
Option B: a custom Micro XRCE-DDS transport over the ADR 0016 RPMsg
channel, with an **unmodified** micro-ROS agent on the A55.

```
   Cortex-M33 (Zephyr)                     Cortex-A55 (Yocto)
 +------------------------+          +---------------------------------+
 | rclc publisher         |          | alp-microros-bridge             |
 |   Micro XRCE-DDS client|  RPMsg   |   <alp/rpc.h> "xrce"  <-> UDP   |
 |   custom transport ----+----------+-> 127.0.0.1:8888 --> micro-ROS  |
 |   (open/close/write/   | ADR 0016 |                       agent     |
 |    read -> rpmsg_link) |          |                        |        |
 +------------------------+          |                  ROS 2 / DDS    |
                                     |            ros2 topic echo ...  |
                                     +---------------------------------+
```

```
examples/multicore/microros-ros2-v2n/
├── board.yaml            (a55_cluster + m33_sm + ipc, same ipc: as rpmsg-v2n)
├── README.md             (this file)
├── linux/                (a55_cluster: alp-microros-bridge)
│   ├── CMakeLists.txt
│   └── src/bridge.c
└── m33_sm/               (m33_sm: Zephyr micro-ROS node)
    ├── CMakeLists.txt
    ├── prj.conf          (CONFIG_MICROROS=y + RPMsg Kconfig)
    ├── testcase.yaml     (build_only, real board)
    └── src/
        ├── main.c        (micro-ROS node + the 4 transport callbacks)
        └── rpmsg_link.[ch]  (OpenAMP plumbing -> "send/recv one datagram")
```

## What it shows

- **The custom-transport hook.** `m33_sm/src/main.c` registers four
  callbacks (open/close/write/read) with `rmw_uros_set_custom_transport`
  and passes `framing = false`, because RPMsg already delimits messages.
  No RMW is written or forked; micro-ROS's `rmw_microxrcedds` is used as
  shipped.
- **One new method name over ADR 0016.** XRCE datagrams travel as
  `<alp/rpc.h>` frames with method `xrce`; the bridge and the M33 do not
  invent a wire schema.
- **The bridge** (`linux/src/bridge.c`) is the only thing we own on the
  A55: it forwards `xrce` frames between `alp_rpc_*` and a UDP socket the
  stock `udp4` agent listens on.
- **A start-order-independent session.** The M33 pings the agent until it
  answers, and re-creates the XRCE session if a publish fails, so the
  agent, the bridge and the M33 can be started in any order.

The RPMsg bring-up in `m33_sm/src/rpmsg_link.c` is the bench-proven
sequence from [`../rpmsg-v2n`](../rpmsg-v2n/) (#683/#697/#2374); the
resource table is compiled straight from `../rpmsg-v2n/m33_sm/src/`.

## Prerequisites

1. The RPMsg/UIO path works: `../rpmsg-v2n` (`m33_sm` + `linux`) echoes
   4/4 on the board (#2374). This example needs the same UIO nodes and
   reserved window; it does not add its own.
2. The M33 image for **this** example is what remoteproc attaches to
   (only one M33 image runs at a time; stop/replace `rpmsg-v2n`'s).
3. A ROS 2 **Humble** environment on the A55 that provides a micro-ROS
   agent and `ros2`. No agent is packaged in `alp-image-edge` yet (#372),
   so use a Humble container on the board with host networking, or the
   `micro_ros_agent` package built from source.

## Build

```bash
cd alp-workspace/alp-sdk/examples/multicore/microros-ros2-v2n
tan build            # both slices; same flow as rpmsg-v2n
```

M33 only, for iteration (needs a west workspace with the
`micro_ros_zephyr_module` group fetched, network access, and the ROS 2
colcon build prerequisites the module documents):

```bash
west build -b alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33 \
    examples/multicore/microros-ros2-v2n/m33_sm
```

## Run

On the A55, three processes; any start order works.

```bash
# 1. micro-ROS agent, stock UDP transport (Humble; container shown --
#    a source-built agent runs the same way: micro_ros_agent udp4 --port 8888)
docker run --rm --net=host microros/micro-ros-agent:humble udp4 --port 8888

# 2. the bridge (RPMsg <-> 127.0.0.1:8888); optional args: agent-ip agent-port
alp-microros-bridge

# 3. any ROS 2 Humble shell that shares the agent's DDS domain
ros2 topic list             # expect /alp_counter
ros2 topic echo /alp_counter std_msgs/msg/Int32
# data: 0
# data: 1   (one per second)
```

The bridge speaks to the agent on loopback only, so the link stays on-SoM
(ADR 0035, security note). Pointing it at a non-loopback agent address
works but is outside the ADR's design.

## What is untested

State of this change, precisely:

- **M33 firmware: not compiled.** Neither the micro-ROS module build
  (colcon cross-compile of rcl/rclc/rmw_microxrcedds, done at CMake
  configure time and needing network + the ROS 2 Python build tools) nor
  the Zephyr link has been run here. `prj.conf` Kconfig beyond
  `CONFIG_MICROROS=y` (`CONFIG_POSIX_API`, stack/heap sizes) is taken
  from the module's own sample conventions and must be confirmed against
  the pinned revision (`cfbddc5e`). API names in `main.c`
  (`rmw_uros_set_custom_transport`, `rmw_uros_ping_agent`, rclc init
  calls) follow the Humble headers and must be confirmed by the first
  build.
- **XRCE MTU vs RPMsg payload.** RPMsg carries at most 491 payload bytes
  (496-byte buffer, 5-byte method header). The XRCE custom-transport MTU
  (`UXR_CONFIG_CUSTOM_TRANSPORT_MTU`, a colcon-time option of the module)
  must be set at or below that. Until it is, `transport_write` refuses
  oversized datagrams and the session will not establish; the value and
  the actual buffer size from the resource table are ADR 0035 bench items
  4 and 5.
- **Bridge on target.** `bridge.c` builds clean with `-Wall -Wextra
  -Wpedantic` on a Linux host; it has not run on the board or against a
  real agent.
- **Not measured:** loss, ordering, latency, restart recovery (ADR 0035
  bench items 2 and 3), and the CM33 RAM/flash cost of `CONFIG_MICROROS`
  (item 4). Only the M33 -> A55 publish direction exists; the reverse
  (A55 publisher, M33 subscriber, ADR bench item 1) is not implemented.
- **CI:** `m33_sm` is `build_only` against the real board, like
  `../rpmsg-v2n/m33_sm`; there is no `native_sim` build.

## Maintainer decisions

- `m33_sm/src/rpmsg_link.c` duplicates the OpenAMP bring-up from
  `../rpmsg-v2n/m33_sm/src/main.c` so the bench-proven example stays
  untouched. Recommended follow-up: factor one shared platform file after
  this example is bench-proven.
- `metadata/libraries/micro-ros.yaml` stays Tier B: the Tier A bar also
  needs a CI lane and a bench result, neither of which exists yet.

## Reference

- [ADR 0035](../../../docs/adr/0035-micro-ros-cross-core-transport.md) --
  why this transport; bench checklist.
- [`../rpmsg-v2n`](../rpmsg-v2n/) -- the RPMsg substrate this rides on.
- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- framed RPC surface.
- [`metadata/libraries/micro-ros.yaml`](../../../metadata/libraries/micro-ros.yaml).
