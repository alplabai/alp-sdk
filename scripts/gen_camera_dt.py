#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate the V2N/V2M Linux camera devicetree fragments and kernel config from
metadata, so adding a camera is a metadata-only change (#2633).

Outputs (committed, DO NOT EDIT BY HAND), in
meta-alp-sdk/recipes-kernel/linux/linux-renesas/:

  e1m-x-evk-<connector>-<module_id>.dtsi   one per carrier camera connector x
                                           camera module that fits it; sets
                                           `aliases { alp-camera<N> = &<sensor>; }`
                                           for connector CAM<N>
  camera-sensors.cfg                       CSI-2 receiver + capture-unit
                                           drivers and every camera module's
                                           sensor driver, built in

The linux-renesas bbappend builds the fragment named by
`ALP_CAMERA_CAM0 = "<module_id>"` into the cam0 dtb.

Inputs (nothing is restated here; every value is read):

  metadata/camera_modules/<id>.yaml   chip, I2C address, oscillator, lanes
  metadata/chips/<chip>.yaml          linux: compatible, kconfig, link_freqs,
                                      supplies, clock_name, reset_property,
                                      endpoint_flags
  metadata/boards/<board>.yaml        camera_connectors with `linux: true` (csi, lanes, i2c,
                                      select / enable / reset macros, supply,
                                      lane_polarity) and the e1m_routes the
                                      macros name (active_low)
  metadata/e1m_modules/E1M-V2*.yaml   pad_routes: which SoC instance (or GD32
                                      bridge line) serves each E1M pad.  Every
                                      V2N/V2M SKU must agree; a disagreement
                                      is an error, not a guess.
  metadata/socs/renesas/rzv2n/n44.json  linux_dt: node labels, PFC function
                                      codes, capture unit, receiver kconfig
  metadata/e1m_modules/v2n/renesas-peripheral-map.tsv  signal -> SoC pad
  metadata/os/linux-kernel-drivers.yaml  which sensor drivers (and lane counts)
                                      the BSP kernel can actually serve: native,
                                      or through an alp patch that EXISTS in
                                      the linux-renesas directory
  <linux-renesas>/e1m-v2n-som.dtsi    the GD32 bridge `gpio-line-names`, which
                                      is where a bridge pad's line number lives

A route the metadata records as `TBD` is left out of the fragment with a
comment saying so; a route kind this generator cannot express is an error.
A module that routes more lanes than the connector carries has no fragment, and
neither has a (module, lane count) no available driver can serve; a sensor
driver that is neither native nor patched in gets no kernel config line.

Usage:

    python3 scripts/gen_camera_dt.py            # (re)write the outputs
    python3 scripts/gen_camera_dt.py --check    # exit 1 on drift
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = Path("meta-alp-sdk/recipes-kernel/linux/linux-renesas")
SOM_DTSI = OUT_DIR / "e1m-v2n-som.dtsi"
SOC = Path("metadata/socs/renesas/rzv2n/n44.json")
KERNEL_DRIVERS = Path("metadata/os/linux-kernel-drivers.yaml")
PERIPHERAL_MAP = Path("metadata/e1m_modules/v2n/renesas-peripheral-map.tsv")
SILICON = "renesas:rzv2n:n44"
SPDX = "// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)"
# No metadata field for the camera control bus speed; every sensor in
# metadata/chips/ runs the standard 400 kHz CCI/SCCB rate.
I2C_HZ = 400000


class GenError(Exception):
    pass


def _load(path: Path, parse):
    """Parse a metadata file; any failure names the file (GenError, never a bare traceback)."""
    try:
        doc = parse(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, yaml.YAMLError) as e:
        raise GenError(f"{path.as_posix()}: {type(e).__name__}: {e}") from e
    if not isinstance(doc, dict):
        raise GenError(f"{path.as_posix()}: expected a mapping at the top level, found {type(doc).__name__}")
    return doc


def _yaml(path: Path) -> dict:
    return _load(path, yaml.safe_load)


def _json(path: Path) -> dict:
    return _load(path, json.loads)


def _pad_routes(root: Path) -> dict[str, dict]:
    """{e1m pad: route} shared by every V2N/V2M SKU; error if two disagree."""
    merged: dict[str, tuple[dict, str]] = {}
    for p in sorted((root / "metadata/e1m_modules").glob("E1M-V2[NM]*.yaml")):
        doc = _yaml(p)
        if doc.get("silicon") != SILICON:
            continue
        for r in doc.get("pad_routes") or []:
            key = {k: r[k] for k in ("dispatch", "dispatch_pin") if k in r}
            old = merged.setdefault(r["e1m"], (key, p.stem))
            if old[0] != key:
                raise GenError(f"{r['e1m']}: pad_routes disagree between {old[1]} {old[0]} and "
                               f"{p.stem} {key}; one fragment serves every V2N/V2M SKU")
    return {k: v[0] for k, v in merged.items()}


def _bridge_lines(root: Path) -> dict[str, int]:
    """{"IO16": 7, ...} from the gd32_gpio node's gpio-line-names."""
    text = (root / SOM_DTSI).read_text(encoding="utf-8")
    m = re.search(r"gpio-line-names\s*=(.*?);", text, re.S)
    if not m:
        raise GenError(f"{SOM_DTSI}: no gpio-line-names")
    names = re.findall(r'"([^"]*)"', re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))
    return {n: i for i, n in enumerate(names) if n}


def _signal_pads(root: Path) -> dict[str, tuple[str, str]]:
    """{signal: (pad, e1m_function)} from the SoM peripheral map."""
    out = {}
    for line in (root / PERIPHERAL_MAP).read_text(encoding="utf-8").splitlines()[1:]:
        c = line.split("\t")
        if len(c) >= 4 and c[0] and not c[0].startswith("#"):
            out[c[0]] = (c[1], c[3])
    return out


def _fmt_hz(v: list[int]) -> str:
    return " ".join(str(x) for x in v)


class Ctx:
    def __init__(self, root: Path):
        self.root = root
        self.soc = _json(root / SOC)["linux_dt"]
        self.routes = _pad_routes(root)
        self.lines = _bridge_lines(root)
        self.pads = _signal_pads(root)
        kernels = _yaml(root / KERNEL_DRIVERS)["kernels"]
        if len(kernels) != 1:
            raise GenError(f"{KERNEL_DRIVERS}: expected exactly one BSP kernel, found {sorted(kernels)}")
        self.kdrivers = {d["compatible"]: d for d in next(iter(kernels.values()))["drivers"]}
        self.chips: dict[str, dict] = {}
        self.modules = {p.stem: _yaml(p) for p in sorted((root / "metadata/camera_modules").glob("*.yaml"))}
        for stem, m in self.modules.items():
            if not isinstance(m.get("chip"), str):
                raise GenError(f"metadata/camera_modules/{stem}.yaml: no `chip`")
            self.chips[m["chip"]] = _yaml(root / "metadata/chips" / f"{m['chip']}.yaml")
        # {module_id: why it got no Linux fragment}; printed by main() and quoted by stale_files().
        self.notes: dict[str, str] = {}

    def has_linux(self, mod: dict) -> bool:
        """A chip without drivers.linux (e.g. an AEN-only sensor) is simply not a Linux camera."""
        return bool((self.chips[mod["chip"]].get("drivers") or {}).get("linux"))

    def linux(self, mod: dict) -> dict:
        return self.chips[mod["chip"]]["drivers"]["linux"]

    def _patched(self, drv: dict) -> bool:
        pats = drv.get("patches") or []
        return bool(pats) and all((self.root / OUT_DIR / f).is_file() for f in pats)

    def _missing_patches(self, drv: dict) -> list[str]:
        return [f for f in drv.get("patches") or [] if not (self.root / OUT_DIR / f).is_file()]

    def driver_available(self, mod: dict) -> bool:
        """The sensor driver is in the kernel: native, or every named patch exists."""
        if not self.has_linux(mod):
            return False
        drv = self.kdrivers.get(self.linux(mod)["compatible"])
        return bool(drv) and (drv["native"] or self._patched(drv))

    def serves_lanes(self, mod: dict) -> bool:
        """Some available driver variant accepts this module's lane count; else say why not."""
        mid = mod["module_id"]
        if not self.has_linux(mod):
            self.notes[mid] = f"chip {mod['chip']} has no drivers.linux block"
            return False
        drv = self.kdrivers.get(self.linux(mod)["compatible"])
        if not drv:
            self.notes[mid] = f"{self.linux(mod)['compatible']} is not in {KERNEL_DRIVERS.name}"
            return False
        if not self.driver_available(mod):
            self.notes[mid] = "patch " + ", ".join(self._missing_patches(drv)) + " absent"
            return False
        lanes = drv.get("native_lanes")
        ok = not drv["native"] or lanes is None or mod["lanes"] in lanes or self._patched(drv)
        if not ok:
            self.notes[mid] = f"{mod['lanes']} lane(s) not served by {drv['compatible']} (native lanes {lanes})"
        return ok

    def soc_route(self, e1m: str, what: str) -> dict:
        r = self.routes.get(e1m)
        if r is None or r.get("dispatch") != "direct":
            raise GenError(f"{what}: {e1m} has no `direct` SoM pad_route ({r})")
        ent = self.soc.get(r["dispatch_pin"])
        if ent is None:
            raise GenError(f"{what}: SoC linux_dt has no {r['dispatch_pin']}")
        return ent


def _gpio(ctx: Ctx, board: dict, macro: str, what: str) -> dict:
    """Resolve a board GPIO macro to {e1m, active_low, kind, line|None}."""
    ent = next((e for e in board["e1m_routes"].get("gpio", []) if e.get("macro") == macro), None)
    if ent is None:
        raise GenError(f"{what}: {macro} is not in the board's e1m_routes.gpio")
    r = ctx.routes.get(ent["e1m"], {})
    out = {"e1m": ent["e1m"], "macro": macro, "active_low": bool(ent.get("active_low")),
           "dispatch": r.get("dispatch")}
    if r.get("dispatch") == "gd32_bridge":
        pad = ent["e1m"].rsplit("_", 1)[1]
        if pad not in ctx.lines:
            raise GenError(f"{what}: {pad} is not a gpio-line-name of {SOM_DTSI.name}")
        out["line"], out["gd32_pin"] = ctx.lines[pad], r.get("dispatch_pin")
    elif r.get("dispatch") != "TBD":
        raise GenError(f"{what}: {ent['e1m']} dispatch {r.get('dispatch')!r} is not supported "
                       "(only gd32_bridge, or TBD which is left out)")
    return out


def render_fragment(ctx: Ctx, board_name: str, board: dict, conn: str, mod: dict) -> str:
    c = board["camera_connectors"][conn]
    lx, chip = ctx.linux(mod), mod["chip"]
    pre, n = conn.lower(), mod["lanes"]
    what = f"{board_name} {conn} + {mod['module_id']}"
    compat = mod.get("linux_compatible", lx["compatible"])
    if compat != lx["compatible"] and compat not in (lx.get("variants") or []):
        raise GenError(f"{what}: linux_compatible {compat!r} is neither {chip}'s drivers.linux.compatible "
                       f"{lx['compatible']!r} nor one of its drivers.linux.variants {lx.get('variants') or []}")
    if mod["xclk_hz"] not in lx.get("xclk_supported_hz", [mod["xclk_hz"]]):
        raise GenError(f"{what}: xclk_hz {mod['xclk_hz']} is not in {chip} linux xclk_supported_hz")
    freqs = None
    if lx.get("link_freqs"):
        freqs = next((f["link_freqs_hz"] for f in lx["link_freqs"] if f["lanes"] == n), None)
        if freqs is None:
            raise GenError(f"{what}: {chip} driver has no link_freqs for {n} lane(s)")

    bus = next((e for e in board["e1m_routes"]["buses"] if e["macro"] == c["i2c"]), None)
    if bus is None:
        raise GenError(f"{what}: {c['i2c']} is not in the board's e1m_routes.buses")
    i2c = ctx.soc_route(bus["e1m"], what)
    csi = ctx.soc_route(c["csi"], what)
    pins = []
    for sig, func in i2c["pinmux"].items():
        pad, fn = ctx.pads[sig]
        m = re.fullmatch(r"P(\d)(\d)", pad)
        if not m:
            raise GenError(f"{what}: pad {pad} of {sig} is not a P<digit port><pin> pad")
        pins.append((f"RZV2N_PORT_PINMUX({m.group(1)}, {m.group(2)}, {func})", fn))

    hogs, not_modelled = [], []
    for sel in c.get("select") or []:
        g = _gpio(ctx, board, sel["gpio"], what)
        if g["dispatch"] == "TBD":
            not_modelled.append(f"{g['macro']} ({g['e1m']}): SoM route is TBD; the mux select is left to the carrier default")
        else:
            hogs.append((g, "output-high" if sel["value"] else "output-low",
                         f" * {g['macro']} = E1M {g['e1m'].rsplit('_', 1)[1]} = GD32 {g['gd32_pin']}, bridge line {g['line']}"))
    reset = None
    for role in ("enable", "reset"):
        if c.get(role):
            g = _gpio(ctx, board, c[role], what)
            if g["dispatch"] == "TBD":
                not_modelled.append(f"{g['macro']} ({g['e1m']}): SoM route is TBD; the camera {role} is not driven from Linux")
            elif role == "enable":
                hogs.append((g, "output-low" if g["active_low"] else "output-high", None))
            else:
                reset = g

    # Hogs read "<role>-hog"; the line name is the macro without its prefix.
    def line_name(g):
        return g["macro"].removeprefix("XEVK_PIN_").lower().replace("_", "-")

    pol = c.get("lane_polarity")
    pol = [pol[0]] + pol[1:1 + n] if pol and any(pol) else None
    lanes = " ".join(str(i + 1) for i in range(n))
    clk_nc = "clock-noncontinuous" in (lx.get("endpoint_flags") or [])
    t = "\t"

    o = [SPDX, "/*",
         f" * GENERATED by scripts/gen_camera_dt.py -- DO NOT EDIT BY HAND.",
         f" * {mod['display_name']} on the {board_name} {conn} connector ({c['refdes']}) ->",
         f" * RZ/V2N MIPI CSI-2 receiver ({csi['label']}) -> {csi['capture']} -> /dev/video*.",
         " *",
         f" * Inputs: metadata/camera_modules/{mod['module_id']}.yaml, metadata/chips/{chip}.yaml,",
         f" * metadata/boards/{board_name}.yaml camera_connectors.{conn}, the E1M-V2N/V2M pad_routes,",
         f" * {SOC.as_posix()} linux_dt.",
         " *",
         " * NOT part of the default dtb: the linux-renesas bbappend builds it into the",
         f" * {pre} dtb when ALP_CAMERA_{conn} = \"{mod['module_id']}\".",
         f" * Link frequency, lanes, supplies and clock follow the {lx['compatible']} driver",
         f" * facts in metadata/chips/{chip}.yaml (drivers.linux).",
         ]
    if mod.get("linux_bench", "unverified") != "verified":
        o += [" *", " * BENCH-UNVERIFIED: this fragment has not been booted with this module."]
    if hogs and any(h[2] for h in hogs):
        o += [" *", " * GD32 bridge lines (line number = index in &gd32_gpio gpio-line-names):"]
        o += [h[2] for h in hogs if h[2]]
    if not_modelled:
        o += [" *", " * Not modelled:"] + [f" *   - {m}" for m in not_modelled]
    o += [" */", "", "/ {",
          f"{t}{pre}_xclk: {pre}-xclk {{",
          f'{t}{t}compatible = "fixed-clock";',
          f"{t}{t}#clock-cells = <0>;",
          f"{t}{t}clock-frequency = <{mod['xclk_hz']}>;",
          f"{t}}};"]
    sup = lx.get("supplies") or []
    if sup and c.get("supply"):
        m = re.fullmatch(r"fixed-(\d)v(\d)", c["supply"])
        if not m:
            raise GenError(f"{what}: supply {c['supply']!r} is not fixed-<N>v<N>")
        uv = (int(m.group(1)) * 10 + int(m.group(2))) * 100000
        o += ["", f"{t}{pre}_supply: regulator-{pre}-{c['supply'].removeprefix('fixed-')} {{",
              f'{t}{t}compatible = "regulator-fixed";',
              f'{t}{t}regulator-name = "{pre}-{c["supply"].removeprefix("fixed-")}";',
              f"{t}{t}regulator-min-microvolt = <{uv}>;",
              f"{t}{t}regulator-max-microvolt = <{uv}>;",
              f"{t}{t}regulator-always-on;",
              f"{t}}};"]
    # <alp/camera.h> camera_id N resolves through this alias to the sensor node
    # (src/backends/camera/yocto_drv.c), then walks the media graph to /dev/video*.
    idx = re.fullmatch(r"CAM(\d+)", conn)
    if not idx:
        raise GenError(f"{what}: connector {conn!r} is not CAM<N>; no alp-camera<N> alias index")
    o += ["", f"{t}aliases {{", f"{t}{t}alp-camera{int(idx.group(1))} = &{pre}_sensor;", f"{t}}};"]
    o += ["};", ""]
    if hogs or reset:
        o += ["&gd32_gpio {"]
        for i, (g, level, _) in enumerate(hogs):
            o += ([""] if i else []) + [
                f"{t}{line_name(g)}-hog {{", f"{t}{t}gpio-hog;",
                f"{t}{t}gpios = <{g['line']} GPIO_ACTIVE_HIGH>;", f"{t}{t}{level};",
                f'{t}{t}line-name = "{line_name(g)}";', f"{t}}};"]
        o += ["};", ""]
    o += ["&pinctrl {",
          f"{t}/* {bus['e1m']} ({c['i2c']}) = {i2c['label']} */",
          f"{t}{i2c['label']}_pins: {i2c['label']} {{",
          ]
    for i, (pm, fn) in enumerate(pins):
        o.append(f"{t}{t}{'pinmux = ' if i == 0 else t + ' '}<{pm}>{';' if i == len(pins) - 1 else ','} /* {fn} */")
    o += [f"{t}}};", "};", ""]
    o += [f"&{i2c['label']} {{",
          f"{t}pinctrl-0 = <&{i2c['label']}_pins>;",
          f'{t}pinctrl-names = "default";',
          f"{t}clock-frequency = <{I2C_HZ}>;",
          f'{t}status = "okay";', "",
          f"{t}{pre}_sensor: camera@{mod['i2c_addr_7bit']:x} {{",
          f'{t}{t}compatible = "{compat}";',
          f"{t}{t}reg = <0x{mod['i2c_addr_7bit']:x}>;",
          f"{t}{t}clocks = <&{pre}_xclk>;"]
    if lx.get("clock_name"):
        o += [f'{t}{t}clock-names = "{lx["clock_name"]}";']
    if sup and c.get("supply"):
        o += [f"{t}{t}{s}-supply = <&{pre}_supply>;" for s in sup]
    if reset:
        o += [f"{t}{t}{lx.get('reset_property', 'reset-gpios')} = <&gd32_gpio {reset['line']} "
              f"{'GPIO_ACTIVE_LOW' if reset['active_low'] else 'GPIO_ACTIVE_HIGH'}>;"]
    o += ["", f"{t}{t}port {{", f"{t}{t}{t}{pre}_sensor_out: endpoint {{",
          f"{t}{t}{t}{t}remote-endpoint = <&{pre}_csi_in>;",
          f"{t}{t}{t}{t}data-lanes = <{lanes}>;"]
    if freqs:
        o += [f"{t}{t}{t}{t}link-frequencies = /bits/ 64 <{_fmt_hz(freqs)}>;"]
    if clk_nc:
        o += [f"{t}{t}{t}{t}clock-noncontinuous;"]
    o += [f"{t}{t}{t}}};", f"{t}{t}}};", f"{t}}};", "};", "",
          f"&{csi['label']} {{", f'{t}status = "okay";', "",
          f"{t}ports {{", f"{t}{t}#address-cells = <1>;", f"{t}{t}#size-cells = <0>;", "",
          f"{t}{t}port@0 {{", f"{t}{t}{t}reg = <0>;",
          f"{t}{t}{t}{pre}_csi_in: endpoint {{",
          f"{t}{t}{t}{t}remote-endpoint = <&{pre}_sensor_out>;",
          f"{t}{t}{t}{t}data-lanes = <{lanes}>;"]
    if pol:
        o += [f"{t}{t}{t}{t}/* clock + data lanes; the connector's `lane_polarity` (rzg2l-csi2 patch 0017). */",
              f"{t}{t}{t}{t}lane-polarities = <{' '.join(str(p) for p in pol)}>;"]
    o += [f"{t}{t}{t}}};", f"{t}{t}}};", f"{t}}};", "};", "",
          f"&{csi['capture']} {{", f'{t}status = "okay";', "};", ""]
    return "\n".join(o)


def render_cfg(ctx: Ctx) -> str:
    rx = ctx.soc["CSI0"].get("kconfig")
    if not rx:
        raise GenError(f"{SOC}: linux_dt.CSI0 has no `kconfig` (receiver + capture drivers)")
    sensors = sorted({ctx.linux(m)["kconfig"] for m in ctx.modules.values() if ctx.driver_available(m)})
    return ("# GENERATED by scripts/gen_camera_dt.py -- DO NOT EDIT BY HAND.\n"
            "# Camera capture stack (linux_dt.CSI0.kconfig in the SoC JSON) and the Linux driver\n"
            "# of every camera module in metadata/camera_modules/ (its chip's drivers.linux.kconfig)\n"
            "# that the BSP kernel carries (metadata/os/linux-kernel-drivers.yaml: native, or its alp\n"
            "# patch is present),\n"
            "# built in (=y, the images install no kernel-modules) on every V2N/V2M machine so any\n"
            "# camera works once its DT is selected.  The receiver / capture nodes stay disabled in\n"
            "# DT unless a camera is selected.\n"
            + "".join(f"{k}=y\n" for k in [*rx, *sensors]))


def generate_with_notes(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    """({path relative to root: content} for every generated file,
        {module_id: why it got no fragment}).  Only GenError escapes."""
    ctx = Ctx(root)
    out: dict[str, str] = {}
    try:
        out[(OUT_DIR / "camera-sensors.cfg").as_posix()] = render_cfg(ctx)
        for bp in sorted((root / "metadata/boards").glob("*.yaml")):
            board = _yaml(bp)
            for conn, c in (board.get("camera_connectors") or {}).items():
                if not c.get("linux"):  # no Linux camera path on this connector (e.g. the E1M-EVK's Zephyr-only CAM0)
                    continue
                for mid, mod in ctx.modules.items():
                    try:
                        if mod["lanes"] > c["lanes"] or not ctx.serves_lanes(mod):
                            continue
                        name = f"{bp.stem}-{conn.lower()}-{mid}.dtsi"
                        out[(OUT_DIR / name).as_posix()] = render_fragment(ctx, bp.stem, board, conn, mod)
                    except (KeyError, TypeError) as e:
                        raise GenError(f"{bp.name} {conn} + metadata/camera_modules/{mid}.yaml: "
                                       f"missing or malformed field {e}") from e
    except (KeyError, TypeError) as e:
        raise GenError(f"missing or malformed field {e}") from e
    return out, ctx.notes


def generate(root: Path) -> dict[str, str]:
    """{path relative to root: content} for every generated file."""
    return generate_with_notes(root)[0]


def stale_files(root: Path, want: dict[str, str], notes: dict[str, str] | None = None) -> list[str]:
    """Committed generated files that differ from, are missing in, or are not
    in `want` (an orphan fragment of a removed module)."""
    msgs = []
    for rel, text in want.items():
        p = root / rel
        if not p.is_file() or p.read_bytes() != text.encode("utf-8"):
            msgs.append(f"{rel} is stale or missing")
    for p in sorted((root / OUT_DIR).glob("e1m-*-cam*-*.dtsi")):
        rel = (OUT_DIR / p.name).as_posix()
        if rel not in want:
            why = next((f" (module {m} skipped: {r})" for m, r in (notes or {}).items()
                        if p.name.endswith(f"-{m}.dtsi")), "")
            msgs.append(f"{rel} is generated for no (connector, module) pair{why}; delete it")
    return msgs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--check", action="store_true", help="exit 1 if a committed output is stale")
    args = ap.parse_args()
    try:
        want, notes = generate_with_notes(args.root)
    except (GenError, OSError) as e:
        print(f"gen_camera_dt: {type(e).__name__}: {e}", file=sys.stderr)
        return 1
    for m, r in notes.items():
        print(f"gen_camera_dt: skipped {m}: {r}", file=sys.stderr)
    bad = stale_files(args.root, want, notes)
    if args.check:
        for m in bad:
            print(f"gen_camera_dt: {m} -- run python3 scripts/gen_camera_dt.py", file=sys.stderr)
        if not bad:
            print(f"OK   {len(want)} camera fragments in sync")
        return 1 if bad else 0
    for rel, text in want.items():
        p = args.root / rel
        if not p.is_file() or p.read_bytes() != text.encode("utf-8"):
            p.write_bytes(text.encode("utf-8"))
            print(f"wrote {rel}")
    for m in bad:
        if m.endswith("delete it"):
            (args.root / m.split()[0]).unlink()
            print(f"removed {m.split()[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
