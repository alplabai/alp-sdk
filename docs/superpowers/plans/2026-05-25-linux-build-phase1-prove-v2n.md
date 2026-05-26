# Linux Build — Phase 1: Prove the V2N path + reconcile docs

> **STATUS — DONE / superseded by reality (2026-05-26).**
> This plan was drafted around a "regenerate the DT patches against AI
> SDK 7.10 + retire the 6.30 narrative" premise that proved **backwards**.
> The validated reality: **BSP v6.30 (linux-renesas 6.1.141-cip43, SHA
> 6717c06c) IS the path** — the carrier DT patches apply to it **as-is,
> no regen** — and the AI-SDK *platform* (7.1) is just a separate umbrella
> axis. This file now records the achieved outcome, not the (obsolete)
> original task steps.

**Goal (as achieved):** the BSP v6.30 `bitbake-layers` flow in
[`../../../meta-alp-sdk/README.md`](../../../meta-alp-sdk/README.md) is the
single documented Linux build path for the V2N carrier, naming both
version axes (platform 7.1 / BSP v6.30) consistently.

## Outcome (validated 2026-05-26 on WSL)

- **DT patches 0006–0013 apply cleanly** to the stock BSP v6.30 kernel
  (linux-renesas 6.1.141-cip43, SHA 6717c06c — the exact kernel the BSP
  ships). No regen, no bbappend retarget; the SRCREV/override matched.
- **`core-image-minimal` baked clean** for `MACHINE=e1m-v2n101-a55` —
  produced the kernel `Image`, the carrier dtb
  `r9a09g056n48-rzv2n-evk.dtb`, the TF-A FIP + u-boot, and a `.wic.gz`.
- **Docs reconciled + kas retired:** `kas/e1m-v2n.yml` deleted;
  `docs/build-yocto-v2n.md`, `docs/cross-platform-setup.md`, and
  `docs/e1m-x-v2n-sdk-integration.md` now point at the README's
  bitbake-layers flow; version language is "platform 7.1 / BSP v6.30"
  everywhere, with no login-gated download links.
- **Overlay fixes the bake surfaced** (machine-conf `require
  conf/machine/rzv2n-evk.conf`, `meta-ros2-humble` → `LAYERRECOMMENDS`,
  `alp-remoteproc.service` packaging) are staged on
  `fix/meta-alp-sdk-v2n-overlay`, pending bench confirmation.

## Remaining before Phase 2

- A full `MACHINE=e1m-v2n101-a55 bitbake alp-image-edge` bake (exercises
  the `.service` packaging + the ROS/DEEPX recipes), and an **on-bench
  boot + carrier smoke test** (model string, `i2cdetect` = i2c-0/1/2/8,
  PHYs attach `stmmac-N:02`, EHCI USB 2.0). Audio stays off until DT
  patch 0010 is converted to the `ti,tas2563` codec (separate carrier
  task). These close Phase 1 and unblock Phase 2 (the `alp build`
  provider wraps this proven flow).
