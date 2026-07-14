# etl-fixed-containers

Builds an `etl::vector<int, 8>` and an `etl::map<const char *, int, 4>`
-- two of the Embedded Template Library's fixed-capacity, STL-shaped
containers -- and demonstrates their headline property for firmware:
**no heap allocation, ever**. Storage for both containers is embedded
inline at compile time, sized by the `MAX_SIZE` template parameter.

**[UNTESTED]** on real silicon -- this example is verified on
`native_sim` only, per the 3rd-party-library examples plan. ETL is
header-only and portable; on-target the same `#include`s and API
calls build and run unchanged.

## What this shows

* `board.yaml`'s `libraries: [etl]` -> the loader adds
  `vendors/etl/include` (the vendored ETL 20.39.4 headers, Task 0)
  and `metadata/library-profiles/etl/etl_profile.h` to the include
  path.
* `etl::vector<int, 8>`: `push_back()`, `size()`, `capacity()`,
  range-based `for` -- same shape as `std::vector`, but the storage
  for all 8 slots lives inside the object, not behind a heap pointer.
* `etl::map<const char *, int, 4>`: `operator[]` insert, `find()`
  lookup -- same shape as `std::map`, backed by a fixed pool of 4
  pre-allocated tree nodes.
* Why capacity violations are safe on firmware: `ETL_NO_EXCEPTIONS`
  (set in the profile header) routes an over-capacity `push_back()`/
  `insert()` through `etl_error_handler` -- a deterministic fault --
  instead of throwing (there's nothing to catch a C++ exception on
  most Cortex-M builds) or silently corrupting memory.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/peripheral-io/etl-fixed-containers \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

```
[etl-fixed-containers] vector: size=5 capacity=8
[etl-fixed-containers] vector: sum=15
[etl-fixed-containers] map: size=3
[etl-fixed-containers] map: lookup("b") = 2
[etl-fixed-containers] done
```

## No-STL vs STL-interop mode

`metadata/library-profiles/etl/etl_profile.h` defines `ETL_NO_STL`
unless the build opts into a real C++ standard library
(`CONFIG_EXTERNAL_LIBCPP` / `CONFIG_GLIBCXX_LIBCPP` /
`CONFIG_LIBCXX_LIBCPP`). This example's `prj.conf` deliberately does
**not** set `CONFIG_REQUIRES_FULL_LIBCPP`, so it builds in
`ETL_NO_STL` mode -- the same standalone configuration ETL runs in on
real Cortex-M silicon, where there is no host STL to interoperate
with. See `prj.conf` for the reasoning.

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
- [`metadata/library-profiles/README.md`](../../../metadata/library-profiles/README.md)
  -- the profile-header mechanism.
- Upstream ETL docs: https://www.etlcpp.com/
