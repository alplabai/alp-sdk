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
- **One harness per parser the SDK itself ships, linking the production
  code.**  A harness links the real `.c` and fakes only the hardware
  seams (bus, GPIO, time).  A parser the SDK does not own (Zephyr's MQTT
  client, BLE host stack, TLS library, the OPTIGA vendor library) gets
  no harness here: fuzzing a hand-written look-alike would only give
  false assurance.
- **No HW dependencies.**  Each harness compiles into a self-
  contained binary that runs anywhere clang runs; the fuzz target
  doesn't open peripherals or talk to a network.

## Harnesses

Every harness links the production code named below.

| File                       | Production code fuzzed                                                                    |
|----------------------------|-------------------------------------------------------------------------------------------|
| `cc3501e_fuzz.c`           | `cc3501e_reply_verdict()` in `chips/cc3501e/cc3501e_core.c`: reply status, proto-4 CRC-16 trailer and dead-phase guard.  Bus, GPIO and time are stubbed. |
| `eeprom_manifest_fuzz.c`   | `alp_hw_info_classify_manifest()` + `alp_hw_info_crc32()` in `src/zephyr/hw_info_zephyr.c`: magic, schema_version, CRC32, field copy. |
| `gd32_bridge_frame_fuzz.c` | `protocol_dispatch` from `gd32-bridge-firmware:src/protocol.c` against arbitrary opcode + payload bytes, cross-checking the firmware `crc16_ccitt_false` against an in-harness reference on every iteration. |
| `mproc_frame_fuzz.c`       | `src/common/proto/alp_mproc_frame.c` decode, plus an encode/decode round trip. |
| `swd_packet_fuzz.c`        | `chips/gd32_swd/gd32_swd.c`: gd32_swd_init / connect / halt / flash_write / verify / reset_and_run against a fuzz-fed SWDIO bit stream (ACK, data word, parity); only the pad seam and `alp_gpio_*` are faked. |
| `update_log_entry_fuzz.c`  | `src/update_log/engine.c` entry decode with the vendored SHA-256. |

## Build

Opt-in via the top-level CMake option `ALP_BUILD_FUZZ=ON`:

```bash
CC=clang CXX=clang++ cmake -B build-fuzz \
    -DALP_OS=yocto \
    -DALP_BUILD_FUZZ=ON
cmake --build build-fuzz --target alp_fuzz_cc3501e alp_fuzz_eeprom_manifest

# Run one harness for 30 seconds (no seed corpus checked in for this
# harness yet -- see "Seed corpora" below; libFuzzer mutates from
# scratch when none is given).
./build-fuzz/tests/fuzz/alp_fuzz_cc3501e -max_total_time=30
```

Clang ≥ 14 is recommended -- earlier versions miss some
`-fsanitize=fuzzer` runtime stubs on aarch64 sysroots.

## Seed corpora

A harness can ship `tests/fuzz/corpus/<name>/` with short seed inputs
that libFuzzer mutates from.  Today only `mproc_frame` and
`update_log_entry` have a corpus directory, and it holds just a
`.gitkeep`: no harness has real seed files checked in.  The cc3501e,
eeprom_manifest and swd_packet harnesses stamp valid structure
(CRC trailer, magic/schema/CRC32, or a bit stream) from a flag byte
instead, so libFuzzer reaches the deep paths without a seed.

## CI integration

`.github/workflows/pr-fuzz-smoke.yml` builds every `alp_fuzz_*` target
and runs each for 20 seconds per PR (it checks out
`alplabai/gd32-bridge-firmware` for the `gd32_bridge_frame` harness).

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
