# vendors/gd32_firmware_library

Vendor wrapper for the **GigaDevice GD32G5x3 firmware library** —
backs the **GD32G553MEY7TR** supervisor MCU on the E1M-X V2N /
V2N-M1 SoMs.  The library carries CMSIS + GigaDevice's standard
peripheral drivers (clock tree, GPIO, SPI slave, I2C slave, ADC,
TIMER, FMC, …) that the `gd32-bridge` firmware links against when
`BRIDGE_HAL_BACKEND=gd32`.

## Pinning + provenance

* Canonical source: **GigaDevice MCU download portal**,
  G5-filtered listing -- <https://www.gd32mcu.com/en/download/7?kw=GD32G5>.
  Two files cover the G5x3 family:

  | Asset                       | Pinned version | Released    | Purpose                                                |
  |-----------------------------|----------------|-------------|--------------------------------------------------------|
  | **GD32G5x3 Firmware Library** | **v1.5.0**   | 2026-02-11  | CMSIS + standard peripheral library (what we link)     |
  | **GD32G5x3 AddOn**           | **v1.3.0**    | 2026-04-15  | Keil CMSIS-Pack + IAR add-on (IDE integration only)    |

  The bridge firmware builds against the **Firmware Library**
  archive; the AddOn pack is an IDE-time convenience for
  developers who want Keil / IAR auto-complete support and is
  optional for the gd32-bridge build.
* GitHub mirrors: as of 2026-05-13 neither
  [CommunityGD32Cores](https://github.com/CommunityGD32Cores/gigadevice-firmware-and-docs)
  nor [zephyrproject-rtos/hal_gigadevice](https://github.com/zephyrproject-rtos/hal_gigadevice)
  carries the G5x3 family yet, so the library is fetched from the
  vendor portal until those mirrors catch up.

The library itself is **not stored in this repo** (matching the
convention applied to every other vendor SDK -- see the
`vendors/*/sdk/` ignore in [`/.gitignore`](../../.gitignore)).
Drop the unpacked archive into this directory using the layout
documented below, and the gd32-bridge build picks it up via
`BRIDGE_HAL_BACKEND=gd32`.

## Drop layout

```
vendors/gd32_firmware_library/
├── README.md                   (this file, tracked)
├── CMakeLists.txt              (TODO: thin wrapper, tracked)
├── sdk/                        (vendor pack -- gitignored)
│   ├── Firmware/
│   │   ├── CMSIS/
│   │   │   ├── Core/Include/    (cmsis_compiler.h, core_cm33.h, ...)
│   │   │   └── GD/GD32G5x3/
│   │   │       ├── Include/
│   │   │       │   └── gd32g5x3.h
│   │   │       └── Source/
│   │   │           ├── system_gd32g5x3.c
│   │   │           └── GCC/
│   │   │               └── startup_gd32g553.S
│   │   └── GD32G5x3_standard_peripheral/
│   │       ├── Include/
│   │       │   └── gd32g5x3_*.h (one per peripheral)
│   │       └── Source/
│   │           └── gd32g5x3_*.c
│   └── Utilities/             (optional vendor demo glue -- not used)
└── Examples/                   (vendor demos -- not built, kept for reference)
```

Files under `sdk/` are gitignored.  Tracked files in this
directory are exactly: `README.md` and (once the wrapper lands)
`CMakeLists.txt`.

## Drop procedure

1. Go to <https://www.gd32mcu.com/en/download/7?kw=GD32G5> and
   download the **GD32G5x3 Firmware Library v1.5.0** archive
   (released 2026-02-11).

2. Drop it into the canonical location.  Two equivalent paths:

   **A.  VS Code (recommended).**  In the workspace, run
   `Tasks: Run Task` -> `GD32 SDK · drop firmware library into vendors/`,
   paste the downloaded archive's path when prompted.  The task
   wraps the same script as path B.

   **B.  Command line.**

   ```bash
   python3 scripts/drop_gd32_firmware_library.py \
       /path/to/GD32G5x3_Firmware_Library_V1.5.0.zip
   ```

   The script searches the archive for
   `Firmware/CMSIS/GD/GD32G5x3/Include/gd32g5x3.h` (its layout
   anchor), copies `Firmware/` and (optional) `Utilities/` into
   `vendors/gd32_firmware_library/sdk/`, and prints the resolved
   library-version triple from the device header so you can
   confirm what landed.

3. Confirm the headers compile against the bridge HAL by building
   the bridge with `BRIDGE_HAL_BACKEND=gd32`:

   ```bash
   cd alp-sdk/gd32-bridge
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake \
       -DBRIDGE_HAL_BACKEND=gd32
   cmake --build build
   ```

   The CMake configure step prints
   `vendors/gd32_firmware_library/sdk: found GD32G5x3 v...` when
   the pack is correctly placed; absent that, the configure fails
   with a clear `not found` message instead of a silent link
   error.

## Licensing

GigaDevice's firmware library ships under their own license terms
(see the header of any file in the unpacked archive).  Do NOT
re-distribute the library through this repo -- the public ALP SDK
keeps the empty `sdk/` directory + this README only.  Downstream
projects that consume the SDK as a packaged artifact (the alp-sdk
release tarball) are responsible for honouring the GigaDevice
license when shipping the bridge firmware to end users.

## When the upstream catches up

When the community mirror starts carrying the G5x3 family, this
directory's wrapper CMakeLists can switch to a git submodule or
west-managed module entry pointing at the upstream.  Until then,
the manual-drop path is the supported flow.
