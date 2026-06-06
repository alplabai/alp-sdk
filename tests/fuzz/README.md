# tests/fuzz/ -- Alp SDK libFuzzer harnesses

Scaffolding for the v1.0 fuzz testing deliverable in
[`VERSIONS.md`](../../VERSIONS.md) ("Fuzz testing on every parser
surface (manifest readers, MQTT, BLE)").  Each harness is a
single libFuzzer entry point (`LLVMFuzzerTestOneInput`) that
exercises one parser surface with corpora-driven input.

## Design

- **libFuzzer + AddressSanitizer + UBSan.**  Standard clang-only
  combo; the build uses `-fsanitize=fuzzer,address,undefined`.
  GCC's `-fsanitize=fuzzer` doesn't exist -- harnesses build only
  with clang.
- **One harness per parser surface.**  Frame-format decoders, JSON
  / metadata loaders, MQTT framing, BLE GATT writes, OTA payload
  signature verification.  v0.3 ships two anchors (`cc3501e` +
  `iot_mqtt`) and v1.0 fills the rest.
- **No HW dependencies.**  Each harness compiles into a self-
  contained binary that runs anywhere clang runs; the fuzz target
  doesn't open peripherals or talk to a network.

## Harnesses shipped (v0.3)

| File                       | Target surface                                                                            |
|----------------------------|-------------------------------------------------------------------------------------------|
| `cc3501e_fuzz.c`           | The `<alp/protocol/cc3501e.h>` SPI wire-protocol frame parser (Alif <-> CC3501E).         |
| `iot_mqtt_fuzz.c`          | MQTT v3.1.1 fixed + variable-length header decoder, as consumed by `<alp/iot.h>`'s MQTT path. |
| `eeprom_manifest_fuzz.c`   | 24C128 EEPROM manifest decoder consumed by `<alp/hw_info.h>` (magic + schema_version + CRC32). |
| `gd32_bridge_frame_fuzz.c` | Drives `protocol_dispatch` from `firmware/gd32-bridge/src/protocol.c` against arbitrary opcode + payload bytes, then cross-checks the firmware-side `crc16_ccitt_false` symbol against an in-harness reference impl on every iteration -- silent CRC drift between the two becomes a libFuzzer crash. |
| `swd_packet_fuzz.c`        | Arm SWD packet header + 32-bit data parity decoder used by `chips/gd32_swd/`.  Still an inline reference parser pending the host driver factoring its parity helpers out as non-static functions; TODO tracked in the file header. |

The `cc3501e_fuzz.c`, `iot_mqtt_fuzz.c`, `eeprom_manifest_fuzz.c`
harnesses still carry an *inline* reference parser -- the SDK
doesn't yet ship a public parser API for any of those three
surfaces, so the fuzz target uses a known-correct decoder that
mirrors what the implementation would do.  v0.4 swaps the inline
parsers for the real ones (`alp_cc3501e_parse_frame`,
`alp_mqtt_decode_fixed_header`, `alp_hw_info_decode_eeprom`) once
those land in the implementation pass.  Catching crashes on the
inline decoder today still has value: it shadows the cc3501e
firmware's parser (separate repo `alplabai/cc3501e-firmware`), so
malformed-frame issues caught here are also reportable upstream.

## Build

Opt-in via the top-level CMake option `ALP_BUILD_FUZZ=ON`:

```bash
CC=clang CXX=clang++ cmake -B build-fuzz \
    -DALP_OS=yocto \
    -DALP_BUILD_FUZZ=ON
cmake --build build-fuzz --target alp_fuzz_cc3501e alp_fuzz_iot_mqtt

# Run one harness for 30 seconds with the seed corpus.
./build-fuzz/tests/fuzz/alp_fuzz_cc3501e -max_total_time=30 tests/fuzz/corpus/cc3501e
```

Clang ≥ 14 is recommended -- earlier versions miss some
`-fsanitize=fuzzer` runtime stubs on aarch64 sysroots.

## Seed corpora

Each harness ships its own `tests/fuzz/corpus/<name>/` directory with
one-shot seed inputs that exercise the happy path.  libFuzzer
mutates from these.  Corpus files are intentionally short (≤ 128 B)
so the fuzzer reaches deep code paths quickly.

The first round of seeds is hand-rolled (one valid frame per
opcode family for cc3501e; one CONNECT + one PUBLISH for MQTT).
v1.0 captures coverage-driven corpus minimisation under
`tests/fuzz/corpus/<name>/min/`.

## CI integration (deferred)

v1.0 wires `pr-fuzz.yml` to run each harness for 60 seconds per
PR using libFuzzer's `-max_total_time` knob.  v0.3 / v0.4 do not
run fuzz in CI -- the harness exists so contributors can fuzz
locally before pushing, and so the v1.0 wiring has a stable
target list.

## OSS-Fuzz integration (deferred)

If we land continuous fuzzing via OSS-Fuzz, each `LLVMFuzzerTestOneInput`
entry point integrates without changes -- that's the same ABI.
The `tests/fuzz/CMakeLists.txt` already emits binaries with the
canonical `alp_fuzz_<name>` naming OSS-Fuzz scrapers expect.

## See also

- [`tests/bench/`](../bench/) -- the microbench suite, the other half
  of the v1.0 hardening prep (task #15).
- [`VERSIONS.md`](../../VERSIONS.md) -- v1.0 deliverables.
- [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md) -- the
  CC3501E wire protocol design (target of `cc3501e_fuzz.c`).
