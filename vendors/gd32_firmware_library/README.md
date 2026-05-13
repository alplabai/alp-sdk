# vendors/gd32_firmware_library

Vendor wrapper for the **GigaDevice GD32G5x3 firmware library** --
backs the **GD32G553MEY7TR** supervisor MCU on the E1M-X V2N /
V2N-M1 SoMs.  The library carries CMSIS + GigaDevice's standard
peripheral drivers (clock tree, GPIO, SPI slave, I2C slave, ADC,
TIMER, FMC, ...) that the `gd32-bridge` firmware links against
when `BRIDGE_HAL_BACKEND=gd32`.

## How the vendor pack lands here

The library lives in this directory's
[`upstream/`](upstream/) **git submodule** pointing at
<https://github.com/alplabai/gd32g5x3-firmware-library> -- a
verbatim mirror of GigaDevice's v1.5.0 archive, redistributed
under **SLA-GD0001 v1.1** (the licence terms quoted in that
repo's README; full licence PDF preserved in the tree).

Clone or refresh the submodule with:

```bash
git submodule update --init --recursive vendors/gd32_firmware_library/upstream
```

Then build:

```bash
cd gd32-bridge
cmake -B build \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake \
      -DBRIDGE_HAL_BACKEND=gd32
cmake --build build
```

The wrapper [`CMakeLists.txt`](CMakeLists.txt) fatal-errors with a
clear "not found" message if the submodule hasn't been checked
out yet, so misconfigurations are diagnosed immediately.

## Why a submodule (and not in the main repo)?

The library is ~38 MB unpacked, ships under GigaDevice's own
licence (SLA-GD0001 v1.1, not Apache-2.0), and is GigaDevice's
intellectual property.  Splitting it into its own repo:

* keeps the alp-sdk main tree under one licence (Apache-2.0) with
  one copyright holder;
* keeps blame, history, and `git log --stat` on alp-sdk clean (no
  five-figure line counts from vendor reformats / version bumps);
* lets the version pin advance independently of the SDK release
  cycle (the submodule SHA is the pin);
* mirrors the existing pattern for other vendor SDKs the project
  uses (Alif HAL, DEEPX SDK, Renesas BSP) -- all consumed by
  reference rather than vendored in-tree.

## Bumping the upstream version

GigaDevice publishes new versions periodically.  When a new v1.6.x
ships:

1. In the
   [gd32g5x3-firmware-library](https://github.com/alplabai/gd32g5x3-firmware-library)
   repo: re-vendor the archive into the tree, tag the result
   (`v1.6.0`), push.  Follow that repo's README "Bumping the
   upstream version" section.

2. In `alp-sdk`:

   ```bash
   cd vendors/gd32_firmware_library/upstream
   git fetch && git checkout v1.6.0
   cd ../../..
   git add vendors/gd32_firmware_library/upstream
   git commit -m "deps(gd32): bump firmware library v1.5.0 -> v1.6.0"
   ```

3. Run a `BRIDGE_HAL_BACKEND=gd32` build; fix any HAL-layer
   breakage in `firmware/gd32-bridge/hal/bridge_hw_gd32.c`.

## Licence + redistribution constraints

Read
[`upstream/SOFTWARE LICENSE AGREEMENT SLA-GD0001-version1.1.pdf`](upstream/SOFTWARE%20LICENSE%20AGREEMENT%20SLA-GD0001-version1.1.pdf)
for the authoritative text.  Quick reference:

* **Source redistribution allowed** (SLA § 2.ii) provided
  GigaDevice's copyright + warranty disclaimer are preserved on
  every file.  The sub-repo preserves them byte-for-byte.
* **Binary redistribution allowed** (SLA § 2.iii) provided the
  resulting binary runs on GigaDevice silicon.  Our consumer
  (`gd32-bridge` firmware) runs on the GD32G553 -- compliant.
* **No sublicensing** outside those permissions.  Downstream forks
  of alp-sdk inherit the SLA.
