# Porting a new E1M SoM to the ALP SDK

> **Read first:** [`docs/e1m-pinout.md`](e1m-pinout.md) explains
> the pinout chain (e1m-spec → per-SoM manifest → studio →
> SDK).  This guide assumes that chain is understood and only
> covers the SDK-side steps.

This guide walks through the steps to add HAL/HW support for a new
E1M-* variant.  It assumes you already have:

- A board file in `alplabai/alp-zephyr-modules` (for Zephyr targets) or
  a Yocto BSP layer (for Linux targets).
- A vendor HAL/SDK that exposes I2C/SPI/GPIO/UART (and any extras the
  variant offers, e.g. MIPI CSI-2).

## 1. Confirm core topology + default OS mapping

A SoM is no longer "a Zephyr SoM" or "a Yocto SoM"; it's a set of
on-die programmable cores, each of which runs a runtime.  Land the
metadata for that topology first — every downstream step assumes
it exists.

1. **Populate `cores[]` in the SoC spec.**  Edit
   `metadata/socs/<vendor>/<family>/<part>.json` and give each
   on-die programmable core a normalized `id` per the §3.3
   nomenclature in
   [`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`](superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md)
   (cluster cores collapse into one key, e.g. `a55_cluster`;
   M-class cores stay separate, e.g. `m33_sm`, `m55_hp`, `m55_he`).
   The `id` must match the regex `^[a-z][a-z0-9_]+$` and be unique
   within the SoC.
2. **Add the per-SoM defaults.**  In
   `metadata/e1m_modules/<SKU>.yaml`, add three blocks:
   - `topology:` — one entry per `cores[].id` declaring the
     default `os:` (+ `app:` / `image:` / `machine:` / `board:` /
     `toolchain:` as appropriate).
   - `memory_map:` — DDR + on-chip-RAM + per-core TCM regions
     with `accessible_from:` so the orchestrator can resolve
     carve-outs.
   - `mailbox:` — controller name + per-channel reservations,
     at minimum one channel `reserved_for: alp_default_rpmsg`.
3. **Update `docs/os-support-matrix.md`.**  Add one column per
   `<core_id>` × runtime pairing (not one per SoM × OS like the
   pre-v0.6 shape).  A V2N port adds two columns: `V2N: a55_cluster
   Yocto` and `V2N: m33_sm Zephyr`.  Mark each library `stub`,
   `planned`, `code complete (untested)`, or `GA` as appropriate.
   Land this row first — it sets the contract for the rest of the
   port.

## 2. Add a vendor wrapper directory

```
vendors/<som-slug>/
├── README.md                 # what this vendor wrapper covers
├── CMakeLists.txt            # builds the wrapper as a static lib
├── i2c.c                     # implements vendors_<slug>_i2c_*()
├── spi.c
├── gpio.c
├── uart.c
└── (display.c, camera.c, ...) # as needed
```

The wrapper functions are **internal** — they are called from
`src/<os>/peripheral.c`.  They are not part of the public surface.
Naming convention: `vendors_<slug>_<peripheral>_<verb>()`.

## 3. Hook the wrapper into each per-core OS backend

Wire the vendor wrapper into the backend(s) for every core class
the SoM exposes — each per-core wiring is a separate step.

- **A-cluster present** (V2N, AEN E5..E8, iMX93)?  Wire
  `src/yocto/peripheral.c` — call into Linux userspace
  (`/dev/i2c-N`, `spidev`, `gpio chardev v2`, `tty*`).
- **M-class peer present** (V2N M33-SM, AEN M55-HP / M55-HE,
  iMX93 M33)?  Wire `src/zephyr/peripheral.c` — route through
  Zephyr drivers first; fall back to vendor calls only when
  Zephyr doesn't expose the feature.
- **Bare-metal slice** (any core with `os: baremetal`)?  Wire
  `src/baremetal/peripheral.c` — call vendor wrappers directly.

A SoM with both an A-cluster and an M-class peer needs both Yocto
and Zephyr wiring; neither half is optional.  Each per-core
backend dispatches on the studio-resolved instance id (`bus_id`,
`pin_id`).  How that id maps to a vendor handle is per-SoM —
typically a static lookup table.

## 4. Update the build glue

- `west.yml`         — add or pin the vendor HAL repo if not already in
  the Zephyr west manifest.
- `cmake/`           — if your SoM requires extra `find_package` logic
  (e.g. a CMSIS DFP location), add a helper module here.
- `zephyr/module.yml` — declare any new Kconfig fragments the SoM exposes.

## 5. Add CI coverage

Update `.github/workflows/` (or the equivalent CI manifest) so the new
SoM is built in the CI matrix.  At minimum:

- A "compiles" job — full ALP SDK builds against the new vendor wrapper.
- A "smoke" job — runs `tests/` under QEMU or the vendor's simulator
  if available.

Real-silicon CI is a stretch goal; document any test runner you wire
up under `tests/README.md`.

## 6. Land per-library implementations

Don't try to ship every library at once.  The expected order is:

1. **Peripherals** — required for every block.
2. **Display** (optional) — only if the SoM is used in display-bearing
   designs.
3. **Camera** (optional) — only if the SoM is used in vision designs.
4. **IoT** (optional) — only if the SoM exposes Wi-Fi or wired networking.
5. **GUI/LVGL** — automatic once Display is GA.
6. **Math / Signal** — usually GA-by-construction (CMSIS-DSP scalar paths).

After each library reaches GA on the new SoM, flip its cell in
`os-support-matrix.md` from `stub`/`planned` to `GA`.

## 7. Write a porting note

Drop a `vendors/<som-slug>/README.md` covering:

- Which vendor HAL version is pinned and where it lives.
- Which peripheral instances exist (`I2C0..I2Cn`, `SPI0..`, GPIO banks).
- Any deviations from the generic Zephyr/Linux mapping.
- Known limitations (e.g. only DMA on SPI0).

This is what the alp-studio pin allocator reads (indirectly, via the
SoM manifest in `alplabai/alp-studio/library/_soms/<id>/manifest.json`)
to decide which peripheral instance to assign each block.
