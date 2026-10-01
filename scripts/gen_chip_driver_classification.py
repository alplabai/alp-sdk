#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate docs/chip-driver-classification.md from metadata/chips/*.yaml.

Issue #500: `driver_status` is catalogue state, not 56 equivalent Alp
implementation bugs.  This report lists every chip whose `driver_status` is
not `complete` and derives, per chip, what the metadata records (status,
silicon verification, buildability, product family, SoM/board demand) plus
an ADR 0017 tier chosen by explicit rules below.  Anything the metadata does
not record is printed as "not recorded in metadata" -- never guessed.

The one non-metadata input is EVIDENCE: for each non-complete chip, what a
reviewer found upstream (Zephyr v4.4.1 tree) or in the vendor ecosystem, and
the remaining gap.  The tier is a function of the evidence KIND, not typed
by hand.  A non-complete chip with no EVIDENCE entry, or an entry for a chip
that is no longer non-complete, FAILS the generator: new non-complete
records cannot enter without a disposition (#500 acceptance criterion 4).

ADR 0017 tier rules (docs/adr/0017-alp-sdk-over-the-vendor-sdk.md):
  upstream_driver   -> T1   upstream Zephyr drives the part; alp-sdk ships
                            DT + the <alp/*> mapping only.
  upstream_binding  -> T1 candidate: only a binding / sibling-family driver
                            exists; part coverage unconfirmed.
  vendor_hw_lib     -> T1.5 thin in-tree shim over a vendor HW library that
                            exposes no Zephyr device (criterion a).
  vendor_stack      -> T2   a genuine vendor driver/stack/runtime exists
                            and is consumed, not rewritten.
  adjacent          -> Adjacent: repo-authored, no upstream equivalent, the
                            chip yaml itself says ADR-0017-ADJACENT.
  unmapped          -> Unmapped: no upstream or vendor driver to consume;
                            the ADR has no tier for an Alp-authored register
                            driver, so the ADR needs a decision.
T3 (SE-mediated) is never assigned: no chip qualifies.

Usage:
    python3 scripts/gen_chip_driver_classification.py          # rewrite
    python3 scripts/gen_chip_driver_classification.py --check  # fail on drift
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "chip-driver-classification.md"
NR = "not recorded in metadata"

# kind -> (tier label, owner rule)
RULES = {
    "upstream_driver": ("T1", "upstream Zephyr (community) + Alp Lab `<alp/*>` mapping"),
    "upstream_binding": ("T1 candidate", "upstream Zephyr (community) + Alp Lab mapping, coverage to confirm"),
    "vendor_hw_lib": ("T1.5", "Alp Lab shim over the vendor library"),
    "vendor_stack": ("T2", "vendor runtime/stack; Alp Lab bring-up glue only"),
    "adjacent": ("Adjacent", "Alp Lab (repo-authored driver)"),
    "unmapped": ("Unmapped", "Alp Lab by default; ADR 0017 decision needed (upstream contribution, accept in-tree, or drop)"),
}
TIER_ORDER = ["T1", "T1 candidate", "T1.5", "T2", "Adjacent", "Unmapped"]

# chip -> (kind, evidence, remaining gap).  Reviewer-recorded input (the
# upstream Zephyr v4.4.1 tree and vendor ecosystem are not in metadata/).
EVIDENCE: dict[str, tuple[str, str, str]] = {
    'act8760': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped) (yaml: no upstream Linux regulator driver either)', 'DVS slots VSET1-3, per-tile fault regs, mV mapping; HIL'),
    'ar0234': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'chip-ID stub only; streaming/MIPI bring-up; HIL'),
    'atecc608b': ('vendor_hw_lib', 'thin in-tree shim over the vendor CryptoAuthLib (Tier 1.5 criterion a: HW library exposing no Zephyr device); library license to confirm', 'wake/idle/sleep only; crypto via CryptoAuthLib import; HIL'),
    'bme280': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/bosch/bme280`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'map `<alp/*>` onto the upstream driver or retire in-tree compensation; HIL'),
    'bmp390': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`dts/bindings/sensor/bosch,bmp390.yaml`); part coverage to confirm (Tier 1 candidate)', 'chip-ID + reset only; pressure read path; HIL'),
    'cc3501e': ('vendor_stack', 'TI CC3501E runs vendor/external bridge firmware; host side is the in-tree SPI protocol client (Tier 2: external vendor firmware)', 'reset() NOSUPPORT until EVK overlay declares WIFI.EN/NRST as alp_gpio lines; HIL'),
    'clk_5l35023b': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'full PLL/OUTDIV/SSC config deliberately left to OTP defaults; HIL'),
    'deepx_dxm1': ('vendor_stack', 'vendor `dx_rt` runtime on Linux/A55 (Tier 2: vendor-consumed; not a Zephyr driver)', 'rail bring-up + runtime out of scope of the chip driver; HIL'),
    'dp83825': ('upstream_driver', 'upstream `phy_ti_dp83825` exists (Tier 1); alp-sdk has no chip driver, the MAC glue is Tier 1.5', 'no driver by design: adopt managed-MDIO path (MDIO_DWMAC_ALIF + upstream phy_ti_dp83825) or record as permanently none'),
    'es8388': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'I2C config shell only; HIL'),
    'gc2145': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/video/gc2145.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'chip-ID stub; switch to upstream video driver; HIL'),
    'gd32_swd': ('unmapped', 'Alp-authored SWD host for the Alp-owned GD32 supervisor; not a vendor driver', 'flips to complete once exercised on real silicon'),
    'gd32g553': ('unmapped', 'Alp-authored bridge host driver + Alp-owned firmware (firmware/gd32-bridge); not a vendor driver', 'see yaml comment: remaining partial reasons are not open issues; silicon coverage'),
    'gdew0154t8': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`drivers/display/ssd16xx.c`); part coverage to confirm (Tier 1 candidate)', 'stub; confirm panel is covered by the ssd16xx family; HIL'),
    'hailo_8l': ('vendor_stack', 'vendor HailoRT runtime on Linux (Tier 2: vendor-consumed)', 'host GPIO sequence only; PCIe + runtime on Linux; HIL'),
    'icm42670': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/tdk/icm42x70`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'accel/gyro/temp read only; retire in-tree or map onto upstream; HIL'),
    'il3820': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`dts/bindings/display/solomon,ssd1608.yaml`); part coverage to confirm (Tier 1 candidate)', 'stub; confirm IL3820 matches the ssd16xx/ssd1608 path; HIL'),
    'ili9341': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/display/display_ili9341.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'init + raw pixel write; use upstream display driver; HIL'),
    'ili9488': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/display/display_ili9488.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'init + raw pixel write; use upstream display driver; HIL'),
    'imx219': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/video/imx219.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'chip-ID stub; use upstream video driver; HIL'),
    'imx296': ('adjacent', 'repo-authored `zephyr/drivers/video/imx296.c`, no upstream driver; yaml calls it ADR-0017-ADJACENT (outside Tiers 1-3)', 'catalogue-only (no chips/ stub); streaming silicon verification partial'),
    'imx335': ('upstream_driver', 'yaml cites ADR 0017 Tier 1: upstream `drivers/video/imx335.c` reused as-is plus one repo patch', 'catalogue-only (none by design); hil_silicon partial'),
    'imx477': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'chip-ID stub; streaming; HIL'),
    'ina236': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`dts/bindings/sensor/ti,ina236.yaml`); part coverage to confirm (Tier 1 candidate)', 'raw read only; confirm upstream ina23x driver coverage; HIL'),
    'lis2dw12': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/st/lis2dw12`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'I2C raw read only; SPI/FIFO deferred; HIL'),
    'lps22hb': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/st/lps22hb`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'stub; use upstream driver; HIL'),
    'lsm6dso': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/st/lsm6dso`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'raw accel/gyro/temp only; HIL'),
    'maxim_max9295_9296': ('vendor_stack', 'no upstream Zephyr driver; a genuine vendor driver/stack exists and is consumed, not rewritten (Tier 2: vendor-consumed) (SerDes pair)', 'stub; defer until a product selects it'),
    'ms5611': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'PROM read + reset only; conversion + compensation; HIL'),
    'murata_lbee0zz2kl': ('vendor_stack', 'vendor Linux/Android/FreeRTOS host stack (Tier 2: vendor-consumed)', 'planned; no Alp chip-driver body; keep metadata-only until a product selects it'),
    'murata_lbee5hy2fy': ('vendor_stack', 'SDIO/BT-UART/I2S paths live in the vendor Linux stack on V2N (Tier 2); the in-tree part is only the REG_ON/HOST_WAKE GPIO surface', 'data paths remain in Yocto/Linux; HIL partial'),
    'murata_lbee5pl2dl': ('vendor_stack', 'vendor Linux/Android host stack (Tier 2: vendor-consumed)', 'planned; metadata-only until a product selects it'),
    'murata_lbes0zz2ll': ('vendor_stack', 'vendor Linux/Android/FreeRTOS host stack (Tier 2: vendor-consumed)', 'planned; metadata-only until a product selects it'),
    'murata_lbes5pl2el': ('vendor_stack', 'vendor Linux/Android/MCUXpresso host stack (Tier 2: vendor-consumed)', 'planned; metadata-only until a product selects it'),
    'optiga_trust_m': ('vendor_hw_lib', 'thin in-tree shim over the Infineon host library `vendors/optiga-trust-m` (Tier 1.5 criterion a); library license to confirm', 'no typed key/crypto calls or PSA driver; Shielded Connection off; HIL partial'),
    'ov2640': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/video/ov2640.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'SCCB chip-ID + reset; use upstream; HIL'),
    'ov5640': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/video/ov5640.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'partial; use upstream; HIL'),
    'ov5645': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'SCCB chip-ID; MIPI lane bring-up; HIL'),
    'ov5647': ('adjacent', 'repo-authored `zephyr/drivers/video/ov5647.c`, no upstream driver; same shape as imx296 (outside Tiers 1-3)', 'catalogue-only (no chips/ stub); streaming silicon verification partial'),
    'ov7670': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`dts/bindings/video/ovti,ov7670.yaml`); part coverage to confirm (Tier 1 candidate)', 'stub; confirm upstream driver coverage; HIL'),
    'ov9281': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'chip-ID stub; yaml hil_silicon is verified for the streaming path but driver_status is not reconciled'),
    'pca9451a': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped); register map cross-checked against the GPL Linux pca9450 driver (clean-room)', 'rail enable/voltage landed (#474); HIL'),
    'quectel_bg77': ('vendor_stack', 'vendor AT-command modem; no upstream Zephyr driver for BG77 (Tier 2: vendor protocol)', 'AT shell only; defer until a product selects it'),
    'quectel_bg95': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`drivers/modem/quectel-bg9x.c`); part coverage to confirm (Tier 1 candidate)', 'AT shell only; map onto upstream modem driver; HIL'),
    'ra8875': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'stub; defer until a product selects it'),
    'rtl8211fdi': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/ethernet/phy/phy_realtek_rtl8211f.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'register layout not validated on this silicon revision; HIL'),
    'semtech_sx1262': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/lora/native/sx126x`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'stub; map onto upstream lora; HIL'),
    'semtech_sx1276': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/lora/loramac-node/sx127x.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'stub; map onto upstream lora; HIL'),
    'ssd1306': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/display/display_ssd1306.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'raw pixel write; `<alp/display.h>` backend; no verification block'),
    'ssd1331': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/display/display_ssd1331.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'framebuffer present; display facade integration; HIL'),
    'st7789': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/display/display_st7789v.c`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'window + write_pixels; fb opt-in; HIL'),
    'tas2563': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'partial config; HIL'),
    'ti_ds90ub953_954': ('vendor_stack', 'no upstream Zephyr driver; a genuine vendor driver/stack exists and is consumed, not rewritten (Tier 2: vendor-consumed) (SerDes pair)', 'stub; defer until a product selects it'),
    'tlv320aic3204': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped) (upstream only has the tlv320dac3100 family)', 'I2C config shell; HIL'),
    'tmc2209': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`dts/bindings/stepper/adi/adi,tmc2209.yaml`); part coverage to confirm (Tier 1 candidate)', 'stub; confirm upstream stepper driver coverage'),
    'ublox_sara_r5': ('upstream_binding', 'upstream Zephyr v4.4.1 ships only a binding / a sibling-family driver (`drivers/modem/ublox-sara-r4.c`); part coverage to confirm (Tier 1 candidate)', 'stub; map onto upstream modem; HIL'),
    'vl53l1x': ('upstream_driver', 'upstream Zephyr v4.4.1 ships a driver for the part (`drivers/sensor/st/vl53l1x`), so alp-sdk should ship only DT + the `<alp/*>` mapping (Tier 1: upstream-native)', 'stub; HIL'),
    'vl53l5cx': ('vendor_stack', 'no upstream Zephyr driver; ST ULD vendor library exists (Tier 2); library license to confirm', 'stub; HIL'),
    'wm8960': ('unmapped', 'neither an upstream Zephyr driver nor a vendor driver exists to consume; ADR 0017 has no tier for an Alp-authored register driver (Unmapped)', 'I2C config shell; audio flow via `<alp/i2s.h>`; HIL'),
}


def _chip_refs(node, found: set[str]) -> None:
    """Collect `chip: <id>` values and `populated:` keys from a YAML tree."""
    if isinstance(node, dict):
        if isinstance(node.get("chip"), str):
            found.add(node["chip"])
        for k, v in node.items():
            if k == "populated" and isinstance(v, dict):
                found.update(x for x in v if isinstance(x, str))
            _chip_refs(v, found)
    elif isinstance(node, list):
        for v in node:
            _chip_refs(v, found)


def _populated(node, chip: str) -> bool | None:
    """Return the board-level `populated: {chip: bool}` value, else None."""
    if isinstance(node, dict):
        p = node.get("populated")
        if isinstance(p, dict) and chip in p:
            return bool(p[chip])
        for v in node.values():
            r = _populated(v, chip)
            if r is not None:
                return r
    elif isinstance(node, list):
        for v in node:
            r = _populated(v, chip)
            if r is not None:
                return r
    return None


def _load(root: Path, sub: str) -> dict[str, dict]:
    return {
        p.stem: yaml.safe_load(p.read_text(encoding="utf-8"))
        for p in sorted((root / "metadata" / sub).glob("*.yaml"))
    }


def _demand(chip: str, soms: dict, boards: dict) -> str:
    parts = []
    on = []
    for sku, d in soms.items():
        found: set[str] = set()
        _chip_refs(d, found)
        if chip in found:
            on.append(sku)
    if on:
        parts.append("SoM: " + ", ".join(f"`{s}`" for s in on))
    for name, d in boards.items():
        found = set()
        _chip_refs(d, found)
        if chip in found:
            pop = _populated(d, chip)
            tag = "DNI/optional" if pop is False else "populated"
            parts.append(f"board `{name}` ({tag})")
    return "; ".join(parts) or "no SoM/board references it (catalogue only)"


def _advance(status: str, hil: str, kind: str) -> str:
    steps = {
        "stub": "implement beyond chip-ID/reset probe",
        "partial": "cover the full datasheet feature set the yaml defers",
        "planned": "a product selects the part, then add a driver body",
        "none": "decide: adopt the upstream/managed path or record as permanently none",
    }[status]
    if kind in ("upstream_driver", "upstream_binding"):
        steps += "; prefer mapping onto the upstream driver over in-tree code"
    if hil != "verified":
        steps += "; silicon verification (hil_silicon -> verified)"
    return steps


def build(root: Path = REPO) -> str:
    chips = _load(root, "chips")
    soms = _load(root, "e1m_modules")
    boards = _load(root, "boards")
    nc = {k: v for k, v in chips.items() if v["driver_status"] != "complete"}
    missing = sorted(set(nc) - set(EVIDENCE))
    stale = sorted(set(EVIDENCE) - set(nc))
    if missing or stale:
        sys.exit(
            "chip-driver-classification: EVIDENCE out of sync with metadata/chips "
            f"(non-complete without a disposition: {missing}; entries for chips "
            f"that are complete/absent: {stale}). Edit EVIDENCE in "
            "scripts/gen_chip_driver_classification.py."
        )
    untracked = sorted(
        c for c in nc
        if EVIDENCE[c][0] != "vendor_stack" and nc[c]["driver_status"] != "planned"
        and not ((nc[c].get("tracking") or {}).get("issue") or (nc[c].get("tracking") or {}).get("blocker"))
    )
    if untracked:
        sys.exit(
            "chip-driver-classification: Alp-owned non-complete chips need "
            f"`tracking.issue` or `tracking.blocker` in metadata/chips/<chip>.yaml: {untracked}"
        )
    rows, tiers, stats = [], {}, {}
    for cid in sorted(nc):
        d = nc[cid]
        kind, ev, gap = EVIDENCE[cid]
        tier, owner = RULES[kind]
        tiers[tier] = tiers.get(tier, 0) + 1
        st = d["driver_status"]
        stats[st] = stats.get(st, 0) + 1
        hil = (d.get("verification") or {}).get("hil_silicon", NR)
        bld = [
            "driver dir yes" if (root / "chips" / cid).is_dir() else "driver dir no",
            "header yes" if (root / "include" / "alp" / "chips" / f"{cid}.h").is_file() else "header no",
            "Kconfig yes" if (d.get("kconfig") or {}).get("zephyr") else "Kconfig no",
        ]
        up = d.get("upstream")
        deps = [x["chip"] for x in d.get("dependencies") or []]
        src = []
        if up:
            src.append("upstream: " + ", ".join(f"{k} `{v}`" for k, v in up.items()))
        if deps:
            src.append("chip deps: " + ", ".join(f"`{x}`" for x in deps))
        tr = d.get("tracking") or {}
        iss = str(tr["issue"]).lstrip("#") if tr.get("issue") else ""
        link = "; ".join(x for x in (f"#{iss}" if iss else "", tr.get("blocker", "")) if x) or NR
        rows.append(
            f"| `{cid}` | `{st}` | {', '.join(bld)} | `{hil}` "
            f"| {', '.join(d.get('families') or [NR])} "
            f"| {_demand(cid, soms, boards)} | {tier}: {ev} | {owner} "
            f"| {'; '.join(src) or NR} | {d.get('license') or NR} | {link} | {_advance(st, str(hil), kind)}; gap: {gap} |"
        )
    sc = ", ".join(f"{k} {stats[k]}" for k in ("partial", "stub", "planned", "none") if k in stats)
    tc = ", ".join(f"{k} {tiers[k]}" for k in TIER_ORDER if k in tiers)
    return f"""<!-- GENERATED by scripts/gen_chip_driver_classification.py from metadata/chips/*.yaml,
     metadata/e1m_modules/*.yaml and metadata/boards/*.yaml. Do not edit by hand;
     run `python3 scripts/gen_chip_driver_classification.py`. -->
# Chip driver classification (non-complete)

Generated for issue #500. Every chip whose `metadata/chips/<part>.yaml` has `driver_status` other than `complete` ({len(nc)} of {len(chips)}). The chip schema carries optional `tracking` and `license` fields. Fields the metadata does not record are marked "{NR}" rather than guessed.

Status counts: {sc}.

Tier counts: {tc}.

## What is derived and from where

- Status, silicon verification (`hil_silicon`), families, upstream/dependency blocks: read from the chip yaml.
- Buildability: presence of `chips/<part>/`, `include/alp/chips/<part>.h` and a `kconfig.zephyr` symbol. Three separate notions are reported and none implies another: `driver_status` is code completeness, the Buildability column is buildability, `hil_silicon` is silicon verification (`ov9281` and `dp83825` show they can disagree).
- Demand: SoM presets (`metadata/e1m_modules`) and board files (`metadata/boards`) that reference the chip. A `populated: false` entry is shown as DNI/optional.
- ADR 0017 tier: a function of the evidence kind (rules in the script header, citing [ADR 0017](adr/0017-alp-sdk-over-the-vendor-sdk.md)). The evidence (upstream Zephyr v4.4.1 tree or vendor ecosystem findings, and the remaining gap) is the one reviewer-recorded input, kept in the script. The generator fails if a non-complete chip has no disposition there.
- Owner: a function of the tier. Licence, blocker and child issue come from the optional `license` and `tracking` (`issue`, `blocker`) chip fields; the generator fails if an Alp-owned (non-T2, non-`planned`) row has neither `tracking.issue` nor `tracking.blocker`.

## Table

| Chip | Status | Buildability | hil_silicon | Families | Product/board demand | ADR 0017 tier and evidence | Owner | Source / dependency | Licence | Blocker / child issue | Evidence to advance `driver_status` |
|---|---|---|---|---|---|---|---|---|---|---|---|
""" + "\n".join(rows) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = build()
    if a.check:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            print(f"{OUT.relative_to(REPO)} is stale; run python3 scripts/gen_chip_driver_classification.py")
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
