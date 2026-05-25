# Linux Build — Phase 1: Prove the V2N 7.10 path + reconcile docs

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make AI SDK 7.10 / Scarthgap the single canonical Linux build path for the V2N carrier, prove it with one green `bitbake alp-image-edge`, and delete the conflicting 6.30/kas narrative.

**Architecture:** No new tooling. Retarget the existing `meta-alp-sdk` carrier artifacts (DT patches 0006–0013, `linux-renesas` bbappend) from AI SDK 6.30 / linux-renesas 6.1-cip43 to AI SDK 7.10 / Scarthgap 5.0.11, and converge the build docs onto `meta-alp-sdk/README.md`'s 7.10 `bitbake-layers` flow.

**Tech Stack:** Yocto (Scarthgap 5.0.11), Renesas RZ/V AI SDK 7.10, bitbake/bitbake-layers, devicetree (dtc), `git format-patch`.

**Environment legend:** 🖥️ DEV-BOX (this Windows checkout) · 🔧 BENCH (Linux/WSL + the gated 7.10 BSP + the rzv2n kernel tree). Only Task 1 is dev-box; Tasks 2–7 are bench-gated.

---

### Task 1 🖥️ — Retire kas + converge docs onto the 7.10 README

**Files:** Delete `kas/e1m-v2n.yml`; rewrite `docs/build-yocto-v2n.md`; modify `docs/e1m-x-v2n-sdk-integration.md`, `meta-alp-sdk/recipes-kernel/linux/linux-renesas_%.bbappend` (header comment only).

- [ ] **Step 1: Delete the kas manifest.**
```bash
git rm kas/e1m-v2n.yml
```

- [ ] **Step 2: Replace `docs/build-yocto-v2n.md`** with a carrier-deploy doc that defers the build to the canonical README:
```markdown
# Building the V2N Linux image

The **canonical** build path for the V2N / V2N-M1 / i.MX93 Linux image is
**[`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md)** — the AI SDK 7.10 /
Scarthgap 5.0.11 `bitbake-layers` flow (free My-Renesas download, package
`RTK0EF0045Z94001AZJ-v1.0.3`).

> Bootloader is production-flashed by ALP; you build only kernel + rootfs.

## Carrier specifics (E1M-X-EVK)
- MACHINE `e1m-v2n101-a55` (or `-v2n102` / `-v2m101` …); image `alp-image-edge`.
- meta-alp-sdk DT patches 0006–0013 turn the in-tree `r9a09g056n48-rzv2n-evk.dts`
  into the carrier dtb the shipped bootloader loads.
- Boot-verify (carrier smoke): model string, `i2cdetect -l` = i2c-0/1/2/8,
  PHYs attach `stmmac-N:02`, EHCI "USB 2.0 started". Audio is off pending the
  TAS2563 DT (carrier follow-up). End-to-end Ethernet needs the MDI
  pair-reversal fix (errata E1).
```

- [ ] **Step 3: Fix the `linux-renesas_%.bbappend` header comment** — replace the "linux-renesas 6.1-cip43 @ AI SDK 6.30, kernel SHA 6717c06c" line with "linux-renesas @ AI SDK 7.10 / Scarthgap 5.0.11", and the STATUS note with "patches REGENERATED against the 7.10 rzv2n kernel (Phase-1 Task 3)". Drop the kas reference. Leave the `SRC_URI:append` body unchanged (Task 4 retargets the override/SRCREV).

- [ ] **Step 4: Fix `docs/e1m-x-v2n-sdk-integration.md`** — gap-5 row + "What's validated vs not": replace `kas/e1m-v2n.yml` references with "the README's 7.10 `bitbake-layers` flow" and "linux-renesas 6.1-cip43 @ AI SDK 6.30 (SHA 6717c06c)" with "AI SDK 7.10 / Scarthgap".

- [ ] **Step 5: Commit.**
```bash
git add -A
git commit -m "docs(v2n): converge Linux build on the 7.10 README; retire kas + 6.30 narrative"
```

---

### Task 2 🔧 — Stand up the 7.10 BSP build environment

Follows `meta-alp-sdk/README.md` §"Build steps". No repo diffs.

- [ ] **Step 1:** Download `RTK0EF0045Z94001AZJ-v1.0.3.zip` (My Renesas, free signup); `unzip`; `tar zxvf src_setup/rzv2n_ai-sdk_yocto_recipe_*.tar.gz`.
- [ ] **Step 2:** `TEMPLATECONF=$PWD/meta-renesas/meta-rz-distro/conf/templates/vlp-v4-conf/ source poky/oe-init-build-env build`.
- [ ] **Step 3:** `bitbake-layers add-layer` for meta-rz-{graphics,drpai,opencva,codecs}, meta-econsys, meta-ros2-humble; then `git clone https://github.com/alplabai/alp-sdk` + `bitbake-layers add-layer ../alp-sdk/meta-alp-sdk`.
- [ ] **Step 4 (verify):** `bitbake-layers show-layers` lists `meta-alp-sdk` + `meta-renesas`; `bitbake-layers show-machines | grep e1m-v2n101-a55`.

---

### Task 3 🔧 — Regenerate DT patches 0006–0013 against the 7.10 rzv2n dts

The carrier *deltas* are unchanged; only patch line-context + the kernel SRCREV differ vs the 6.30 kernel. Exact diffs are derived here against the 7.10 tree.

**Delta reference (what each patch must still express):**
| Patch | Delta |
|------|------|
| 0006 | `model = "ALP e1m-x carrier + v2n-m1 SoM …"`; `compatible = "alp,e1m-x-v2n-m1", "renesas,r9a09g056n48", …` |
| 0007 | disable EVK-only: `raa215300` (PMIC), `adv7535`+`dsi0`+`du`+`hdmi-out`, `ov5645_csi20/21`+`csi20/21`+`cru0/1`, `pcie0` |
| 0008 | swap eth phy → `rtl8211fdi` |
| 0009 | correct eth phy id → RTL8211F-VD (`0x001c.c878`) |
| 0010 | audio off: disable `da7212`, `rcar_sound`, `sound_card` (TAS2563 DT is a separate carrier task) |
| 0011 | disable unrouted `RIIC3/6/7` |
| 0012 | disable USB-OVC: `/delete-node/ ovc` from `usb20_pins`+`usb30_pins` + gpio-hog P9.6/PB.1 as GPIO in |
| 0013 | set eth phy `reg = <2>` on both phy nodes |

- [ ] **Step 1 (per patch, in the 7.10 `linux-renesas` source tree):** `git apply --reject 000N-*.patch`; for any `.rej`, re-apply the delta above by hand against the 7.10 `arch/arm64/boot/dts/renesas/r9a09g056n48-rzv2n-evk.dts`; `git add -A && git commit`.
- [ ] **Step 2:** `git format-patch -13 --start-number 6 -o <out>`; overwrite the `0006-0013` files under `meta-alp-sdk/recipes-kernel/linux/linux-renesas/`.
- [ ] **Step 3 (verify dtc-clean):** `bitbake linux-renesas -c compile`; `fdtdump` the dtb → `model = "ALP e1m-x carrier…"`, no dtc errors.
- [ ] **Step 4: Commit** the regenerated patches.

---

### Task 4 🔧 — Retarget the bbappend SRCREV/override to 7.10

- [ ] **Step 1:** `bitbake -e linux-renesas | grep -E "^SRCREV=|^PV=|^MACHINEOVERRIDES="` → note the 7.10 values + whether `e1m-v2n101-a55`'s overrides still include `e1m-v2n101`.
- [ ] **Step 2:** In `linux-renesas_%.bbappend`, adjust the `SRC_URI:append:<override>` key if the 7.10 MACHINEOVERRIDES differ.
- [ ] **Step 3 (verify):** `bitbake -e linux-renesas | grep -E "tas2563-audio.cfg|0006-"` shows them in `SRC_URI`.
- [ ] **Step 4: Commit.**

---

### Task 5 🔧 — Recipe-parse gate

- [ ] **Step 1:** `bitbake-layers parse-recipes` (or `bitbake -p`) → no parse errors from meta-alp-sdk.
- [ ] **Step 2:** `bitbake alp-image-edge -n` → task graph resolves.

---

### Task 6 🔧 — Green bake

- [ ] **Step 1:** `MACHINE=e1m-v2n101-a55 bitbake alp-image-edge`.
- [ ] **Step 2 (fix loop):** resolve recipe/layer/fetch breakage until it bakes.
- [ ] **Step 3 (verify):** `tmp/deploy/images/e1m-v2n101-a55/` has `alp-image-edge-*.wic[.gz]`, `Image`, `renesas/r9a09g056n48-rzv2n-evk.dtb`.

---

### Task 7 🔧 — HiL boot smoke + close out

- [ ] **Step 1:** Flash + boot on the bench; carrier smoke: `cat /proc/device-tree/model` = "ALP e1m-x carrier…"; `dmesg | grep -i over-current` = none; `i2cdetect -l` = i2c-0/1/2/8; `ethtool end0` → PHY attaches `stmmac-N:02`. `aplay -l` = no card yet (expected; TAS DT is the carrier follow-up).
- [ ] **Step 2:** Update `meta-alp-sdk/README.md` "Verification status" `[UNTESTED]` → "V2N101 image baked + booted on 7.10 (date)"; commit.

**Phase 1 exit:** the README's 7.10 flow bakes + boots `alp-image-edge` on V2N101; the 6.30/kas narrative is gone. Unblocks Phase 2 (the `alp build` provider wraps this proven flow).
