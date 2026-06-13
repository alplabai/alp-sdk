# Customer-grade SoM image architecture — composable, identified, reproducible

**Status:** Draft under finalization · decisions 1–4 locked 2026-06-13 · decisions 5–7 await owner sign-off
**Owner:** Alp Lab
**Branch:** `feat/customer-grade-edge-image` (off `dev`)
**Affects:** `meta-alp-sdk/recipes-images/*`, `conf/distro/alp.conf`, the `alp-sdk`/`alp-chips`/`alp-perception` recipes, `pr-bitbake.yml`

---

## 1. Motivation

The first successful `alp-image-edge` build (`e1m-v2n101-a55`, 2026-06-13) surfaced three things unfit for a customer-shipped, OTA-updated SoM:

1. **ROS 2 + DDS is welded into every image** — `alp-image-common.inc` installs `rclcpp` + the message/transport stack + `alp-perception` unconditionally, so *both* `edge` and `prod` ship the full ROS/DDS payload. A SoM whose value is the on-device AI runtime (`alp-sdk` + DRP-AI/DEEPX) should not force a robotics framework on every customer.
2. **The image version steals the vendor's number** — `DISTRO_VERSION="6.30"` is the *Renesas RZ/V2N AI SDK BSP* version, but the banner reads "Alp SDK 6.30", implying an Alp SDK 6.30 that does not exist (the SDK is 0.7.0).
3. **Source revisions float** — `alp-sdk`/`alp-chips`/`alp-perception` use `SRCREV = "${AUTOREV}"`, so two builds of "the same" image differ and the Mender artifact name (which bakes `PV`) misrepresents what shipped.

This spec defines the target: a **composable**, **clearly-identified**, **reproducible** image line.

## 2. Target architecture

### 2.1 Composable image taxonomy

One headless core; capabilities are opt-in feature groups composed via `IMAGE_FEATURES`. **A new capability lands as a packagegroup — never an edit to an image recipe.**

| Image | = | Posture |
|---|---|---|
| `alp-image-base` | core only (headless) | composition primitive / minimal appliance |
| `alp-image-edge` | base + `alp-camera` + `alp-display` + `alp-ros` + `debug-tweaks` | dev kitchen-sink |
| `alp-image-prod` | base + `alp-camera` + `alp-display` + hardening (no `alp-ros`) | vision appliance, shippable |

Feature groups (`packagegroup-alp-*`, mapped via `FEATURE_PACKAGES_<feature>`):

| Feature | Packagegroup | Contents |
|---|---|---|
| (core, always) | — (in `common.inc`) | `alp-sdk` + NPU runtime (machine-gated), `mender-client`, watchdog, networkd, entropy, openssh |
| `alp-camera` | `packagegroup-alp-camera` | `libcamera` + GStreamer base |
| `alp-display` | `packagegroup-alp-display` | Weston/Wayland + `libdrm` |
| `alp-ros` | `packagegroup-alp-ros` | `rclcpp` + msg/transport stack + `alp-perception` |

NPU backend selection stays **silicon-determined** in the machine `.conf` (DEEPX gated on `ALP_ENABLE_DEEPX_DXM1`; DRP-AI3 via sysroot), so the *same* image recipe builds for any SoM in the family — a new SoM is a machine `.conf` + an NPU runtime, no image change.

### 2.2 Versioning & identity — three distinct, attributed facts

Never collapse the product version, the upstream BSP, and the Yocto lineage into one label.

| Fact | Value | Role |
|---|---|---|
| Alp SDK / product version | `0.7.0` | **Headline** — release, support, OTA artifact name |
| Upstream BSP (vendor-attributed) | `Renesas RZ/V2N AI SDK BSP 6.30` | Provenance / traceability |
| Yocto lineage | `scarthgap` | Distro foundation |

```sh
# /etc/os-release  (customer-facing banner)
NAME="Alp SDK"
VERSION="0.7.0"
VERSION_ID="0.7.0"

# /etc/alp-release  (provenance manifest — explicit, vendor-attributed)
ALP_SDK_VERSION=0.7.0
ALP_BSP_VENDOR=Renesas
ALP_BSP_NAME="RZ/V2N AI SDK BSP"
ALP_BSP_VERSION=6.30
ALP_YOCTO_RELEASE=scarthgap
ALP_MACHINE=e1m-v2n101-a55
```

**One source of truth = the Alp SDK release tag.** `os-release VERSION`, the `alp-sdk` recipe `PV`, and `MENDER_ARTIFACT_NAME` all derive from it. **Future-proof:** the vendor-attributed BSP line changes vendor/version when Renesas bumps the BSP or a new SoM uses a different SoC vendor (Alif AEN, NXP i.MX93); the Alp version line stays coherent.

### 2.3 Reproducibility

`prod`/release builds pin `alp-sdk`/`alp-chips`/`alp-perception` `SRCREV` to the SDK release tag; only `alp-image-edge` floats `AUTOREV`. The artifact name then truthfully reflects the shipped revision.

### 2.4 Rootfs robustness (prod)

`alp-image-prod` ships a **read-only rootfs** + a writable **data partition** (Mender's `MENDER_DATA_PART`), with the A/B slot commit gated on a **post-boot health check** so a bad OTA rolls back instead of bricking. Cryptographic rootfs integrity (dm-verity) is excluded here and folds in with the verified-boot workstream (decision 5). `alp-image-edge` stays read-write for bench convenience.

## 3. Decisions — all locked 2026-06-13

1. **Composable taxonomy** — `base`/`edge`/`prod` via `IMAGE_FEATURES`.
2. **ROS is opt-in** — `alp-ros` feature group; off by default in `prod`.
3. **Versioning/identity** — SDK 0.7.0 headline + attributed Renesas BSP provenance (§2.2).
4. **Reproducible `prod`** — `SRCREV` pinned to the release tag; `edge` floats.
5. **OTA signing + verified boot → separate security workstream** (not in this pass). Mender artifact signing, FIT verified-boot phase-2 enforcement, and **dm-verity** are sequenced together later, gated on production **key custody** (owner decision). This pass leaves the FIT-signing phase-1 scaffold untouched.
6. **`prod` rootfs: read-only + writable data partition + health-check-gated A/B commit** (§2.4). The robust fixed-function default — survives power-loss and bad OTA. dm-verity is deliberately *excluded here* (it couples to the verified-boot workstream in decision 5) and folds in when that lands.
7. **Compliance: license manifest only.** Keep the SPDX SBOM (already on) + add a human-readable license manifest in the rootfs. No GPLv3-free variant unless a customer mandates it.

## 4. Implementation plan

| Slice | Content | Status |
|---|---|---|
| 1 | packagegroups + `common.inc` decouple + `alp-image-base` + `edge`/`prod` recompose | drafted on branch, **validation pending** |
| 2 | versioning/identity: `os-release` + `/etc/alp-release` manifest, `DISTRO_VERSION` → SDK | not started |
| 3 | `SRCREV` pinning for `prod`/release | not started |
| 4 | read-only rootfs + data partition + health-check-gated A/B (decision 6) + license manifest (decision 7) | not started |

**Deferred to a separate security workstream (decision 5):** Mender artifact signing, FIT verified-boot phase-2 enforcement, dm-verity, production key custody.

**Validation:** per-image `bitbake -g`/dry-run resolve (assert `base` pulls *no* ROS packages; `edge` pulls `alp-perception`) + a full `bitbake` per MACHINE.

## 5. Compatibility

No legacy compat is owed (no active customers). `alp-image-edge` keeps its current everything-on payload, so existing bench flows are unchanged; `alp-image-base` and the ROS-free `prod` are additive.

## 6. Related gap — CI cannot build these yet

`pr-bitbake.yml` cannot build any `alp-image-*` on a clean runner: it only `add-layer`s `meta-alp-sdk` (which then fails its `openembedded-layer` dep), never stages the ROS/Mender layers, and never applies the private TF-A DDR overlay. Fixing the gate to add those layers + run the overlay step (the "CM receives both repos" flow) must land alongside this work so CI actually validates the taxonomy. The private TF-A DDR overlay mechanism itself is unchanged (public bbappend + private `.c` via the gitignored sparse overlay).
