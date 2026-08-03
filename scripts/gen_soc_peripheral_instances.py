#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Project per-instance peripheral register data from a vendored Zephyr SoC
devicetree into a `metadata/socs/**.json` file's `peripheral_instances`
block (issue #1154).

`metadata/socs/**.json` `peripherals:` is a COUNT map (`"i2c": 9`) -- fine
for capability gating (`gen_soc_caps.py`'s `ALP_SOC_*_COUNT`) but useless to
anything that wants to actually INSTANTIATE a peripheral: two consumers
already hardcode per-instance addresses by hand
(`metadata/renode/renesas_rzv2n.repl:61` for CPG/ICU, `:71` for RIIC8 /
BRD_I2C). This generator makes that data mechanical instead of hand-copied,
for the classes where a real, already-west-managed source carries it.

Source of truth (RZ/V2N n44 only, today): the M33 board devicetree
(`zephyr/boards/alp/e1m_v2n101_m33_sm/..._cm33.dts`) `#include`s Zephyr
upstream's own per-SoC devicetree, `dts/arm/renesas/rz/rzv/r9a09g056.dtsi`,
which carries real `reg` / `interrupts` per peripheral node. This script
reads that same file out of the west-managed Zephyr checkout (never a
second hand-transcribed copy) and mechanically extracts `reg` (base
address, size) + `interrupts` (irq number, priority, name) for every node
whose `compatible` is in `_CLASSES` below.

**n44 vs n48 -- why the n48-labelled DTSI is the right source for n44.**
`metadata/socs/renesas/rzv2n/n44.json`'s own `variants[].notes` (order code
R9A09G056N44GBG) already states the reason: Zephyr upstream only models
the n48gbg die (no n44gbg `SOC_*` Kconfig symbol) because the n44/n48
delta is GPU/ISP/crypto fusing only -- devicetree-identical for the M33
peripherals this generator projects. Sourcing M33 peripheral facts from
the n48gbg-labelled DTSI for the n44 SoC spec is therefore correct, not a
substitution of convenience; it would be wrong for any GPU/ISP/crypto
field, none of which this generator touches.

**Coverage is deliberately partial -- read this before trusting a green
run.** `r9a09g056.dtsi` was read node-by-node (not sampled) for this
change. Of n44.json's 27 `peripherals:` keys, only FOUR are projected:

  * `i2c`             <- i2c0..i2c8   (9 renesas,rz-riic nodes)  -- exact
  * `uart`            <- sci0..sci9   (10 renesas,rz-sci-b nodes) -- exact
  * `timer_32bit_gpt` <- gpt0..gpt15  (16 renesas,rz-gpt nodes)  -- exact
  * `timer_32bit_gtm` <- gtm0..gtm7   (8 renesas,rz-gtm nodes)   -- exact

"Exact" means the DTSI node count equals the `peripherals:` count for that
key, so an instance list can stand in for the count with no semantic
change. Two more classes DO have DTSI nodes but at a DIFFERENT
granularity than their `peripherals:` count, and are deliberately left
UNPROJECTED rather than emitting a count-mismatched list a reader could
mistake for complete:

  * `adc_12bit` (24) -- the DTSI models 3 renesas,rz-adc-e UNITS
    (adc0..adc2, each `channel-available-mask = <0xff>` = 8 channels);
    3 x 8 = 24 matches the COUNT, but 3 != 24, so a naive one-entry-per-
    DTSI-node list would silently read as "24 instances -> 3".
  * `gpio` (86) -- the DTSI models 12 renesas,rz-gpio PORTS (gpio0..
    gpio11) whose `ngpios` sum to 86 pins; same unit-vs-leaf mismatch.

The remaining 21 keys (`i3c`, `spi`, `scif`, `i2s`, `spdif`, `pdm`,
`ethernet_1g`, `usb_2`, `usb_3_2_gen2`, `sdio_emmc`, `sdio`, `can_fd`,
`mipi_csi2`, `mipi_dsi`, `pcie_gen3`, `temp_sensor`, `timer_32bit_cmtw`,
`watchdog`, `rtc`, `irq_external`, `dmac_channels`) have NO node in this
devicetree at all -- Zephyr's M33 view simply does not model them (VCD,
ISP, GPU, DRP-AI and several bus/timer blocks are absent by design; `spi`
in particular is a board-level SCI-Simple-SPI *mode* on some `uart`
channels, not a distinct SoC-level DT compatible, so it is not invented
here either). Every skipped key is printed by `main()` on every run so
the gap is never silent.

**No `clocks`.** `r9a09g056.dtsi` was checked for `clocks = <&cpg ...>`
on every node in scope; it has NONE -- not even a raw phandle. Per the
no-inventing-values rule, no `clocks` field is emitted anywhere in
`peripheral_instances`; this is a stronger absence than "phandle without
a frequency mnemonic".

**Not covered by this generator at all:** `metadata/renode/
renesas_rzv2n.repl`'s hand-placed CPG/ICU window (0x40400000, the `intc`
node, compatible `renesas,rz-intc-v2`) is deliberately left alone -- it
is not a `peripherals:` key, and retiring the repl's hand-placement onto
generated data is scoped as separate follow-up work, not this change.

Usage:

    python3 scripts/gen_soc_peripheral_instances.py            # regenerate in place
    python3 scripts/gen_soc_peripheral_instances.py --check    # fail if out of sync

Needs a resolvable Zephyr checkout (same convention as
`scripts/check_toolchain_lock.py`'s `_resolve_zephyr_dir`): `$ZEPHYR_BASE`,
falling back to the west-workspace topdir's conventional `zephyr/`
directory. Skips cleanly (prints `skipped: ...`, exit 0) in both modes
when neither resolves to a real checkout -- this must stay a no-op on a
machine that has never run `west init`, exactly like
`check_emit_kconfig_contract.py`.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# One entry per (SoC spec, DTSI) pair. Deliberately a list, not a single
# hardcoded pair, so a second SoC (e.g. the AEN/Alif family a future
# #948 / #1029 slice targets) is a new entry here, not a new script.
_TARGETS: list[dict] = [
    {
        "soc_json": REPO / "metadata" / "socs" / "renesas" / "rzv2n" / "n44.json",
        "dtsi_relpath": Path("dts") / "arm" / "renesas" / "rz" / "rzv" / "r9a09g056.dtsi",
        # compatible string -> (peripherals: key it feeds, human label)
        "classes": {
            "renesas,rz-riic": "i2c",
            "renesas,rz-sci-b": "uart",
            "renesas,rz-gpt": "timer_32bit_gpt",
            "renesas,rz-gtm": "timer_32bit_gtm",
        },
        # Known DTSI-modelled-but-NOT-projected classes, with why -- surfaced
        # in the skip log so the gap reads as a deliberate decision, not an
        # oversight. See the module docstring for the full explanation.
        "granularity_mismatch": {
            "renesas,rz-adc-e": "adc_12bit (DTSI models 3 units x 8ch each; "
                                 "peripherals.adc_12bit=24 counts channels)",
            "renesas,rz-gpio-common": "gpio (DTSI models 12 ports; "
                                       "peripherals.gpio=86 counts pins)",
        },
    },
]

# Matches every 2-tab-indented `label: name@addr { ... };` node directly
# under `soc { ... }` -- deliberately anchored on the closing brace's exact
# indent depth (two tabs) so a node's own nested child sub-nodes (`pwm {`,
# `uart {`, `counter {` at three tabs, e.g. gpt0's `pwm { ... };`) do not
# terminate the match early; a child closes at three tabs, never two.
_NODE_RE = re.compile(
    r"\n\t\t(?P<label>[a-z][a-z0-9_]*): \S+@[0-9a-fA-F]+ \{\n"
    r"(?P<body>.*?)"
    r"\n\t\t\};",
    re.DOTALL,
)
_COMPATIBLE_RE = re.compile(r'compatible\s*=\s*"([^"]+)"')
_CHANNEL_RE = re.compile(r"channel\s*=\s*<(\d+)>")
_REG_RE = re.compile(r"reg\s*=\s*<\s*(0x[0-9a-fA-F]+|\d+)\s+([^>]+?)\s*>")
_SIZE_MACRO_RE = re.compile(r"DT_SIZE_K\((\d+)\)")
_INTERRUPTS_RE = re.compile(r"interrupts\s*=\s*(?P<cells>.*?);", re.DOTALL)
_INTERRUPT_NAMES_RE = re.compile(r'interrupt-names\s*=\s*(?P<names>"[^;]+");')
_IRQ_CELL_RE = re.compile(r"<\s*(\d+)\s+(\d+)\s*>")
_QUOTED_RE = re.compile(r'"([^"]*)"')


def _resolve_zephyr_dir() -> Path:
    """Same resolution as `check_toolchain_lock.py::_resolve_zephyr_dir`:
    `$ZEPHYR_BASE`, falling back to the west-workspace topdir's
    conventional `zephyr/` project directory."""
    env_base = os.environ.get("ZEPHYR_BASE")
    return Path(env_base) if env_base else REPO.parent / "zephyr"


def _parse_int(token: str) -> int:
    return int(token, 16) if token.lower().startswith("0x") else int(token)


def _parse_size(token: str) -> int | None:
    """Return a byte size from a `reg` cell's second field, or None if it
    is neither a bare literal nor a recognised `DT_SIZE_K(n)` macro (a
    macro this script doesn't know stays unresolved rather than guessed)."""
    token = token.strip()
    m = _SIZE_MACRO_RE.fullmatch(token)
    if m:
        return int(m.group(1)) * 1024
    try:
        return _parse_int(token)
    except ValueError:
        return None


def _parse_node(label: str, body: str) -> dict | None:
    """Extract the grounded facts this generator projects from one node
    body. Returns None if the node has no `reg` (nothing to project)."""
    reg_m = _REG_RE.search(body)
    if not reg_m:
        return None
    base = _parse_int(reg_m.group(1))
    size = _parse_size(reg_m.group(2))

    channel_m = _CHANNEL_RE.search(body)
    index = int(channel_m.group(1)) if channel_m else None

    names: list[str] = []
    names_m = _INTERRUPT_NAMES_RE.search(body)
    if names_m:
        names = _QUOTED_RE.findall(names_m.group("names"))

    interrupts: list[dict] = []
    irqs_m = _INTERRUPTS_RE.search(body)
    if irqs_m:
        cells = _IRQ_CELL_RE.findall(irqs_m.group("cells"))
        for i, (irq, prio) in enumerate(cells):
            entry = {"irq": int(irq), "priority": int(prio)}
            if i < len(names):
                entry["name"] = names[i]
            interrupts.append(entry)

    instance: dict = {"index": index if index is not None else 0, "label": label, "base": base}
    if size is not None:
        instance["size"] = size
    if interrupts:
        instance["interrupts"] = interrupts
    return instance


def _extract_instances(dtsi_text: str, classes: dict[str, str],
                        granularity_mismatch: dict[str, str]) -> tuple[dict[str, list[dict]], list[str]]:
    """Return ({peripherals-key: [instance, ...]}, [skip-log lines])."""
    by_key: dict[str, list[dict]] = {key: [] for key in classes.values()}
    seen_compatibles: set[str] = set()
    for m in _NODE_RE.finditer(dtsi_text):
        label, body = m.group("label"), m.group("body")
        compat_m = _COMPATIBLE_RE.search(body)
        if not compat_m:
            continue
        compatible = compat_m.group(1)
        seen_compatibles.add(compatible)
        key = classes.get(compatible)
        if key is None:
            continue
        instance = _parse_node(label, body)
        if instance is None:
            continue
        instance["compatible"] = compatible
        by_key[key].append(instance)

    for instances in by_key.values():
        instances.sort(key=lambda i: i["index"])

    skips: list[str] = []
    for compatible, reason in granularity_mismatch.items():
        if compatible in seen_compatibles:
            skips.append(f"skip {compatible}: granularity mismatch -- {reason}")
        else:
            skips.append(f"skip {compatible}: not found in this DTSI revision "
                          f"(was found when this generator was written)")
    return by_key, skips


def _compact(obj) -> str:
    return json.dumps(obj, separators=(", ", ": "))


def _render_block(by_key: dict[str, list[dict]]) -> str:
    """Hand-roll byte-stable JSON text for the `peripheral_instances` key,
    one instance per line -- mirrors gen_pinmux_capability.py's
    hand-rolled-YAML rationale: deterministic across Python/json versions,
    and it keeps the rest of a hand-formatted SoC spec file untouched
    (a full `json.dump` round-trip of the whole file would reflow every
    hand-aligned block elsewhere in it -- see the PR description)."""
    lines = ['  "peripheral_instances": {']
    keys = list(by_key.keys())
    for ki, key in enumerate(keys):
        instances = by_key[key]
        lines.append(f'    "{key}": [')
        for ii, inst in enumerate(instances):
            comma = "," if ii < len(instances) - 1 else ""
            lines.append(f"      {_compact(inst)}{comma}")
        key_comma = "," if ki < len(keys) - 1 else ""
        lines.append(f"    ]{key_comma}")
    lines.append("  },")
    return "\n".join(lines) + "\n"


def _find_block(text: str, key: str) -> tuple[int, int] | None:
    """Return (start, end) char offsets of a top-level `  "<key>": { ... }`
    block, end being just past its trailing `,\\n` (or `\\n` if it's the
    last key), via brace counting -- robust to nested braces inside the
    block (the `peripherals` block itself has none, but this generator's
    own `peripheral_instances` block does)."""
    header = f'  "{key}": {{'
    start = text.find(header)
    if start == -1:
        return None
    depth = 0
    i = start + len(header) - 1  # index of the opening '{'
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                if text[end:end + 1] == ",":
                    end += 1
                if text[end:end + 1] == "\n":
                    end += 1
                return start, end
        i += 1
    return None


def _splice(text: str, by_key: dict[str, list[dict]]) -> str:
    block = _render_block(by_key)
    existing = _find_block(text, "peripheral_instances")
    if existing is not None:
        start, end = existing
        return text[:start] + block + text[end:]
    peripherals = _find_block(text, "peripherals")
    if peripherals is None:
        raise SystemExit('gen_soc_peripheral_instances: no `"peripherals": {` '
                          "block found to insert after")
    _start, end = peripherals
    return text[:end] + block + text[end:]


def _process(target: dict, zephyr_dir: Path, check: bool) -> tuple[bool, list[str]]:
    """Returns (ok, log lines). ok is False only on a real drift/error;
    a clean environment skip is reported separately by main()."""
    dtsi_path = zephyr_dir / target["dtsi_relpath"]
    soc_json = target["soc_json"]
    try:
        rel = soc_json.relative_to(REPO)
    except ValueError:
        rel = soc_json  # e.g. a test's tmp_path fixture, outside REPO
    if not dtsi_path.is_file():
        return False, [f"MISSING {dtsi_path} (resolved from {zephyr_dir})"]

    dtsi_text = dtsi_path.read_text(encoding="utf-8")
    by_key, skips = _extract_instances(dtsi_text, target["classes"],
                                        target["granularity_mismatch"])

    soc_doc = json.loads(soc_json.read_text(encoding="utf-8"))
    counts = soc_doc.get("peripherals") or {}
    problems = []
    for key, instances in by_key.items():
        want = counts.get(key)
        if want is not None and len(instances) != want:
            problems.append(f"{rel}: {key} has {len(instances)} DTSI instance(s) "
                             f"but peripherals.{key} = {want}")
        if not instances:
            problems.append(f"{rel}: {key} -- expected DTSI nodes for this "
                             f"class, found none (compatible list changed?)")

    log = [f"  {s}" for s in skips]
    if problems:
        return False, [f"FAIL {rel}"] + [f"  {p}" for p in problems] + log

    current_text = soc_json.read_text(encoding="utf-8")
    new_text = _splice(current_text, by_key)

    n_instances = sum(len(v) for v in by_key.values())
    if check:
        if current_text != new_text:
            return False, [
                f"STALE {rel}: peripheral_instances out of sync with "
                f"{dtsi_path} -- run `python3 scripts/gen_soc_peripheral_instances.py`",
            ] + log
        return True, [f"OK   {rel}  ({n_instances} instances across "
                       f"{len(by_key)} classes, in sync)"] + log

    if current_text != new_text:
        soc_json.write_text(new_text, encoding="utf-8", newline="")
        return True, [f"wrote {rel}  ({n_instances} instances across "
                       f"{len(by_key)} classes)"] + log
    return True, [f"OK   {rel}  ({n_instances} instances, already in sync)"] + log


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                     help="fail (exit 1) if any target is out of sync with its DTSI")
    args = ap.parse_args()

    zephyr_dir = _resolve_zephyr_dir()
    if not zephyr_dir.is_dir():
        print(f"skipped: gen_soc_peripheral_instances needs a Zephyr checkout "
              f"(resolved {zephyr_dir}, not a directory) -- set $ZEPHYR_BASE "
              f"or run from a bootstrapped west workspace")
        return 0

    ok = True
    for target in _TARGETS:
        target_ok, lines = _process(target, zephyr_dir, args.check)
        ok = ok and target_ok
        for line in lines:
            print(line)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
