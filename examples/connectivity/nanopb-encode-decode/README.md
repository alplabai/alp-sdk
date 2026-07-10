# nanopb-encode-decode

`[UNTESTED]` -- native_sim build+run PASSED locally (see below), but this
has not been verified on real silicon (no bench/HIL sweep).

Teaches **nanopb** (<https://github.com/nanopb/nanopb>), a small-code-size
Protocol Buffers implementation for embedded targets. `src/main.c`
`pb_encode()`s a tiny `DeviceStatus{int32 uptime_s; string device_id}`
struct into a caller-owned byte buffer, `pb_decode()`s it back into a
fresh struct, and asserts the round trip.

## What this shows

* `pb_ostream_from_buffer()` / `pb_istream_from_buffer()` -- nanopb never
  allocates its own I/O buffer; you own it and size it (here,
  `DeviceStatus_size`, the generator's computed worst-case length).
* A bounded `string` field (`[(nanopb).max_size = 17]` in the `.proto`)
  becomes a plain `char[17]` struct member, not a `pb_callback_t` --
  no heap, no callback plumbing, the whole message is stack-safe.
* Checking `pb_encode()`/`pb_decode()`'s `bool` return + `PB_GET_ERROR()`
  before trusting the output.

## Library integration

nanopb is a **real, fetched module** at
`/home/caner/modules/lib/nanopb` (tag `0.4.9`, zlib license) -- not the
`vendors/nanopb/` stub headers, which are a *different*, SDK-internal
placeholder for `<alp/mproc.h>` IPC framing (see
[`vendors/nanopb/README.md`](../../../vendors/nanopb/README.md); those
stubs are not used by this example).

`board.yaml`'s `cores.m55_hp.libraries: [nanopb]` only emits
`CONFIG_ALP_NANOPB_SW=y` (nanopb has no HW-accelerator class -- see
`metadata/library-profiles/nanopb/hw-backends.yaml`). It does **not**
pull the upstream runtime into the build: the fetched module's
`zephyr/module.yml` declares `cmake-ext`/`kconfig-ext` (an external-glue
extension point alp-sdk does not wire centrally today; the SDK's own
`zephyr/CMakeLists.txt`, a shared file, is out of scope for a
single-example change). `CMakeLists.txt` here routes around that gap by
compiling `pb_common.c` / `pb_encode.c` / `pb_decode.c` directly from
`${ZEPHYR_NANOPB_MODULE_DIR}` -- a CMake variable Zephyr's module loader
sets for *every* discovered module regardless of the `cmake-ext` flag
(`zephyr/cmake/modules/zephyr_module.cmake`).

### `.proto` codegen

`src/device_status.proto` is the schema (source of truth). This build
environment has no `protoc`, so `src/device_status.pb.h` /
`src/device_status.pb.c` are **pre-generated and checked in** -- do not
hand-edit them; regenerate after any `.proto` change with:

```bash
# one-time: a venv with grpcio-tools (bundles a protoc binary) + protobuf
python3 -m venv /tmp/nanopb-venv
/tmp/nanopb-venv/bin/pip install grpcio-tools protobuf
source /tmp/nanopb-venv/bin/activate

python -m grpc_tools.protoc \
    -I examples/connectivity/nanopb-encode-decode/src \
    -I /home/caner/modules/lib/nanopb/generator/proto \
    --plugin=protoc-gen-nanopb=/home/caner/modules/lib/nanopb/generator/protoc-gen-nanopb \
    --nanopb_out=examples/connectivity/nanopb-encode-decode/src \
    examples/connectivity/nanopb-encode-decode/src/device_status.proto
```

## board.yaml HW swap

`som.sku: E1M-AEN801` / `preset: e1m-evk`. This example does no I/O --
swap `som.sku` to any SoM/preset pair and it still builds; the round
trip is entirely in-RAM.

## Build

```bash
west build -b native_sim/native/64 examples/connectivity/nanopb-encode-decode \
    -- -DEXTRA_ZEPHYR_MODULES="$(pwd);/home/caner/modules/lib/nanopb"
west build -t run
```

## Expected output

```
[nanopb-encode-decode] encoded 14 bytes
[nanopb-encode-decode] decoded uptime_s=42 device_id="e1m-aen801"
[nanopb-encode-decode] round trip OK
[nanopb-encode-decode] done
```
