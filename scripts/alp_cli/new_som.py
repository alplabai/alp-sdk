"""`alp new-som` -- scaffold the metadata for porting a brand-new SoM.

The vendor-N+1 porting kit: generates the two metadata skeletons a new
SoM port needs (the SoM preset YAML and, when absent, the SoC spec
JSON), with every schema-required hardware-fact field present as an
explicit TBD placeholder -- values are NEVER invented -- and prints the
numbered porting checklist from docs/porting-new-som.md.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import click

REPO_ROOT = Path(__file__).resolve().parents[2]
SOM_SCHEMA_PATH = REPO_ROOT / "metadata" / "schemas" / "som-preset-v1.schema.json"
SOC_SCHEMA_PATH = REPO_ROOT / "metadata" / "schemas" / "soc-spec-v1.schema.json"

_SKU_RE = re.compile(r"^E1M-[A-Z0-9-]+$")
_SOC_REF_RE = re.compile(r"^[a-z0-9-]+:[a-z0-9-]+:[a-z0-9-]+$")
_FAMILY_RE = re.compile(r"^[a-z][a-z0-9-]*$")
_CORE_ID_RE = re.compile(r"^[a-z][a-z0-9_]+$")

# Canonical backend keys already known to the device dispatcher, plus
# the schema-legal lowercase `tbd` placeholder (the schema pattern
# `^[a-z][a-z0-9_]*$` does not admit the uppercase TBD literal here).
INFERENCE_BACKENDS = ("ethos_u", "drpai", "deepx_dxm1", "tbd")
ETHOS_U_VARIANTS = ("u55", "u65", "u85")

DEFAULT_CORES = ("tbd_core0",)
DEFAULT_BOARD = "E1M-EVK"
DEFAULT_HW_REV = "r1"


def _split_csv(_ctx, _param, value):
    if value is None:
        return None
    return tuple(s.strip() for s in value.split(",") if s.strip())


def _fail(message: str) -> None:
    """Print a CLI error to stderr and exit non-zero."""
    click.echo(f"alp new-som: {message}", err=True)
    raise SystemExit(1)


def _yaml_dquote(value: str) -> str:
    r"""Escape ``value`` for embedding in a YAML double-quoted scalar.

    YAML double-quoted style uses C-like escapes, so `\` and `"` are the
    only characters that need escaping here (control characters are
    rejected up front by the command's validation).
    """
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _current_sku_pattern() -> str:
    schema = json.loads(SOM_SCHEMA_PATH.read_text(encoding="utf-8"))
    return schema["properties"]["sku"]["pattern"]


def _known_board_names() -> set[str] | None:
    """Board ``name:`` values from metadata/boards/, or None if unknowable.

    Returns None (skip the check, keep it a checklist item) when PyYAML
    is unavailable or the SDK boards directory is missing -- the closed
    set of shared carrier boards lives in the SDK checkout, not under
    --output-root.
    """
    try:
        import yaml
    except ImportError:
        return None
    boards_dir = REPO_ROOT / "metadata" / "boards"
    if not boards_dir.is_dir():
        return None
    names: set[str] = set()
    for path in sorted(boards_dir.glob("*.yaml")):
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        except yaml.YAMLError:
            continue
        if isinstance(doc, dict) and isinstance(doc.get("name"), str):
            names.add(doc["name"])
    return names or None


def _family_hw_revisions(sku: str,
                         output_root: Path) -> tuple[Path, set[str]] | None:
    """(path, rev keys) of the SKU family's hw-revisions.yaml, or None.

    None means "not resolvable at scaffold time" -- a brand-new family
    with no SKU-prefix mapping / no hw-revisions file yet (creating one
    is a porting-checklist step), or PyYAML / alp_project unavailable.
    The SKU-prefix -> family-directory map is owned by
    scripts/alp_project.py; it is imported, never duplicated here.
    """
    try:
        import yaml
        from alp_project import _sku_family
    except ImportError:
        return None
    try:
        family_dir = _sku_family(sku)
    except ValueError:
        return None  # brand-new family: no directory mapping yet
    for root in (output_root, REPO_ROOT):
        path = root / "metadata" / "e1m_modules" / family_dir / "hw-revisions.yaml"
        if path.is_file():
            try:
                doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            except yaml.YAMLError:
                return None
            revs = doc.get("hw_revisions")
            if isinstance(revs, dict):
                return path, {str(k) for k in revs}
            return None
    return None


def _interactive(
    sku: str | None,
    soc_ref: str | None,
    family: str | None,
    vendor: str | None,
    display_name: str | None,
    inference_backend: str | None,
    ethos_u_variant: str | None,
    cores: tuple[str, ...] | None,
    default_board: str | None,
    default_hw_rev: str | None,
):
    import questionary

    if sku is None:
        sku = questionary.text(
            "New SoM SKU (e.g. E1M-XYZ101):"
        ).unsafe_ask().strip()
    if soc_ref is None:
        soc_ref = questionary.text(
            "Silicon triple-colon ref (vendor:family:part, e.g. nxp:imx9:imx95):"
        ).unsafe_ask().strip()
    if family is None:
        family = questionary.text(
            "Human-readable family slug (e.g. nxp-imx9):"
        ).unsafe_ask().strip()
    if vendor is None:
        vendor = questionary.text(
            "Vendor display name for the SoC JSON:",
            default=soc_ref.split(":")[0] if _SOC_REF_RE.match(soc_ref) else "",
        ).unsafe_ask().strip()
    if display_name is None:
        display_name = questionary.text(
            "Display name:",
            default=f"{sku} ({vendor} -- scaffold, silicon facts TBD)",
        ).unsafe_ask().strip()
    if inference_backend is None:
        inference_backend = questionary.select(
            "Inference backend (silicon-determined; pick `tbd` when unknown):",
            choices=list(INFERENCE_BACKENDS),
        ).unsafe_ask()
    if inference_backend == "ethos_u" and ethos_u_variant is None:
        ethos_u_variant = questionary.select(
            "Primary Ethos-U variant:", choices=list(ETHOS_U_VARIANTS)
        ).unsafe_ask()
    if cores is None:
        answer = questionary.text(
            "Canonical core ids, comma-separated (leave the tbd placeholder when unknown):",
            default=",".join(DEFAULT_CORES),
        ).unsafe_ask()
        cores = tuple(s.strip() for s in answer.split(",") if s.strip())
    if default_board is None:
        default_board = questionary.text(
            "Default board (stock carrier):", default=DEFAULT_BOARD
        ).unsafe_ask().strip()
    if default_hw_rev is None:
        default_hw_rev = questionary.text(
            "Default hw rev:", default=DEFAULT_HW_REV
        ).unsafe_ask().strip()
    return (sku, soc_ref, family, vendor, display_name, inference_backend,
            ethos_u_variant, cores, default_board, default_hw_rev)


def _render_preset(
    sku: str,
    soc_ref: str,
    family: str,
    display_name: str,
    inference_backend: str,
    ethos_u_variant: str | None,
    cores: tuple[str, ...],
    default_board: str,
    default_hw_rev: str,
) -> str:
    vendor_slug, family_slug, part_slug = soc_ref.split(":")
    soc_rel = f"metadata/socs/{vendor_slug}/{family_slug}/{part_slug}.json"

    lines: list[str] = []
    a = lines.append
    a(f"# Stock preset skeleton for {sku}, generated by `alp new-som`.")
    a("#")
    a("# Every value below marked TBD awaits the authoritative hardware")
    a("# config (datasheet / schematic / BOM).  Fill facts in from the")
    a("# primary source named next to each field -- NEVER guess (see")
    a("# docs/porting-new-som.md).")
    a("")
    a("schema_version: 1")
    a("")
    a("# SKU is assigned by Alp Lab product planning.  A brand-new family")
    a("# also needs one alternation added to the `sku:` pattern in")
    a("# metadata/schemas/som-preset-v1.schema.json (porting guide step 3).")
    a(f"sku: {sku}")
    a("")
    a("# Human-readable family slug (NOT the directory name; the SKU-prefix")
    a("# -> family-directory map lives in scripts/alp_project.py).")
    a(f"family: {family}")
    a("")
    a(f"# Triple-colon silicon ref; must resolve to {soc_rel}.")
    a(f"silicon: {soc_ref}")
    a("")
    a("# Vendor order code from the datasheet's Ordering Information table;")
    a(f"# must match a variants[].order_code in {soc_rel}.")
    a("# While TBD, the loader falls back to variants[].alp_module_skus")
    a("# reverse lookup.")
    a("silicon_variant: TBD")
    a("")
    a(f'display_name: "{_yaml_dquote(display_name)}"')
    a("")
    a("# Populated on-module chips -- the module BOM / schematic is")
    a("# authoritative.  Chip values are slugs matching a")
    a("# metadata/chips/<chip>.yaml manifest.  The legal role keys are the")
    a("# CLOSED set in som-preset-v1.schema.json `on_module:` (pmic_main,")
    a("# pmic_secondary, clock_generator, rtc_external, temperature_sensor,")
    a("# eeprom, secure_element, wifi_ble, supervisor_mcu, ethernet_phy,")
    a("# nor_flash, emmc, npu, pcie_mux, ospi_memories, hyperram,")
    a("# i2c_devices).  Delete the TBD rows for roles this module does NOT")
    a("# populate; add rows for the ones it does.")
    a("on_module:")
    a(f"  silicon:              {soc_ref}")
    a("  pmic_main:            TBD                    # main PMIC part (BOM)")
    a("  eeprom:               TBD                    # identity EEPROM part (BOM)")
    a("  # i2c_devices:                               # per-bus I2C device tables;")
    a("  #   <bus_name>:                              # bus names are a per-family")
    a("  #     bus_master: TBD                        # fact (schematic).  Chip")
    a("  #     devices:                               # slugs + 7-bit addresses come")
    a('  #       - { chip: <slug>, role: <role>, address_7bit: "TBD" }')
    a("")
    a("# Off-SoC module memory in the canonical cross-family shape.")
    a("# Authoritative source: the module BOM (DRAM + flash parts).")
    a("# On-die SRAM/MRAM is derived from the SoC variant, never declared here.")
    a("memory:")
    a("  dram_mbit:            TBD                    # external volatile memory, Mbit")
    a("  flash_mbit:           TBD                    # external non-volatile memory, Mbit")
    a("")
    a("# Inference-accelerator selection -- silicon-determined (SoC")
    a("# datasheet), never customer-facing.  `tbd` is the schema-legal")
    a("# placeholder backend; scripts/check_inference_backend_parity.py")
    a("# cross-checks the value against the device dispatcher and accepts")
    a("# `tbd` ONLY while `status.preliminary:` below is true, so this")
    a("# scaffold commits green as-is.  Replace `tbd` with the real")
    a("# canonical key (ethos_u, drpai, deepx_dxm1, ...) before clearing")
    a("# the preliminary flag.")
    a("inference:")
    a(f"  preferred_backend:    {inference_backend}")
    if inference_backend == "ethos_u":
        a(f"  ethos_u_variant:      {ethos_u_variant}")
        a("  npu_population:")
        a("    # One row per populated NPU instance.  The instance count is")
        a("    # authoritative from the SoC JSON capabilities.ethos_uNN_count;")
        a("    # role labels + host-core pairings come from the vendor RM ->")
        a("    # TBD until ingested, never invented.")
        a(f"    - {{ variant: {ethos_u_variant}, role: TBD, paired_with: TBD }}")
    a("")
    a("# capabilities: -- OPTIONAL, omitted in the skeleton.  Declare ONLY")
    a("# keys the SoM ADDS on top of the silicon capabilities (e.g. an")
    a("# on-module secure element or bridge accelerator); silicon caps come")
    a(f"# from {soc_rel} `capabilities:`.")
    a("")
    a("# Per-core default runtime + app mapping.  Keys MUST match cores[].id")
    a(f"# in {soc_rel} (cross-checked by pr-metadata-validate).  Rename any")
    a("# tbd placeholder id in lockstep with the SoC JSON, then fill each")
    a("# entry: `machine:` + `toolchain:` for Yocto A-class cores, `board:`")
    a("# + `toolchain:` for Zephyr M-class cores (see E1M-AEN801.yaml /")
    a("# E1M-V2N102.yaml for the two shapes).")
    a("topology:")
    for core in cores:
        a(f"  {core}: {{}}")
    a("")
    a("# Memory layout (SRAM banks + on-die flash) is derived from the SoC")
    a("# variant resolved via `silicon_variant:` -- see the SoC JSON")
    a("# `variants[].sram_banks_kb`.  Declare a memory_map: block here ONLY")
    a("# for non-stock partitioning.")
    a("")
    a("# Vendor mailbox / IPC controller -- the vendor reference manual /")
    a("# hand-written HW config is authoritative.  The channel reservations")
    a("# below are the SDK-standard convention shared by every released")
    a("# preset; keep them unless the controller has fewer channels.")
    a("mailbox:")
    a("  controller: TBD")
    a("  channels:")
    a("    - { id: 0, reserved_for: alp_default_rpmsg }")
    a("    - { id: 1, reserved_for: app }")
    a("    - { id: 2, reserved_for: app }")
    a("    - { id: 3, reserved_for: power_mgmt }")
    a("")
    a("# pad_routes: -- OPTIONAL, omitted in the skeleton.  Once the")
    a("# schematic is available, either (a) list every E1M pad that routes")
    a("# through an on-module mediator chip (see E1M-V2N102.yaml), (b)")
    a("# declare an explicit empty list to assert \"no mediator, everything")
    a("# is SoC-direct\" (see E1M-NX9101.yaml), or (c) list pads with")
    a("# `dispatch: TBD` while routing is pending.  Omitted, the loader")
    a("# treats every pad as SoC-direct -- resolve this before clearing")
    a("# status.partial_hw_config.")
    a("")
    a("# helper_firmware: -- OPTIONAL, omitted in the skeleton.  One entry")
    a("# per independently-flashed on-module helper MCU image (see")
    a("# E1M-V2N102.yaml `gd32_bridge`).  Omit when the module has none.")
    a("")
    a("# Must resolve against metadata/e1m_modules/<family-dir>/hw-revisions.yaml.")
    a(f"default_hw_rev:         {default_hw_rev}")
    a("")
    a("# Stock carrier board this SoM ships on (see metadata/boards/).")
    a(f"default_board:          {default_board}")
    a("")
    a("# Keep both flags true until every TBD above is resolved from the")
    a("# authoritative HW config.  `preliminary: true` is also the marker")
    a("# that lets the parity gate accept the `tbd` inference backend.")
    a("status:")
    a("  preliminary:          true")
    a("  partial_hw_config:    true")
    a("")
    return "\n".join(lines)


def _soc_skeleton(sku: str, soc_ref: str, vendor: str,
                  cores: tuple[str, ...]) -> dict:
    _, family_slug, part_slug = soc_ref.split(":")
    core_rows = []
    for core in cores:
        # `count: 1` is the schema minimum, NOT a datasheet fact -- the
        # notes[] entry below flags it (JSON has no comments; this file
        # uses the schema-sanctioned `_pending_reason` + `notes` fields).
        core_rows.append({"id": core, "type": "TBD", "count": 1})
    return {
        "soc_spec_version": 1,
        "ref": soc_ref,
        "vendor": vendor,
        "family": family_slug,
        "part": part_slug,
        "status": "preliminary",
        "pending_reference_manual_ingestion": True,
        "_pending_reason": (
            "Scaffolded by `alp new-som`: every silicon fact in this file "
            "is a placeholder pending datasheet / reference-manual "
            "ingestion.  peripherals: {} means unknown, not zero."
        ),
        "cores": core_rows,
        "npus": [],
        "peripherals": {},
        "variants": [
            {
                "order_code": "TBD",
                "notes": (
                    "Placeholder row: replace order_code with the vendor's "
                    "Ordering Information part number, then mirror it in the "
                    "SoM preset's silicon_variant."
                ),
                "alp_module_skus": [sku],
            }
        ],
        "notes": [
            "JSON has no comments -- scaffold TODOs live in this notes[] "
            "array plus _pending_reason (both schema-sanctioned fields).",
            "cores[]: the tbd id / type TBD / count 1 rows are "
            "schema-minimum placeholders, NOT datasheet facts; rename the "
            "ids in lockstep with the SoM preset's topology: keys.",
            "vendor/family/part hold the ref slugs until the datasheet "
            "display names are filled in.",
        ],
    }


def _schema_errors(doc, schema_path: Path) -> list[str]:
    """Validate ``doc`` against ``schema_path``; return error strings."""
    try:
        import jsonschema
    except ImportError:
        return []  # self-check skipped; validate_metadata.py is the gate
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    return [
        f"{'/'.join(str(p) for p in err.absolute_path) or '<root>'}: {err.message}"
        for err in sorted(validator.iter_errors(doc),
                          key=lambda e: list(e.absolute_path))
    ]


@click.command(
    name="new-som",
    help="Scaffold the metadata skeletons for porting a new SoM.",
)
@click.option("--sku", default=None, help="New SoM SKU (e.g. E1M-XYZ101).")
@click.option("--soc-ref", default=None,
              help="Silicon triple-colon ref, e.g. nxp:imx9:imx95.")
@click.option("--family", default=None,
              help="Human-readable family slug, e.g. nxp-imx9.")
@click.option("--vendor", default=None,
              show_default="the soc-ref vendor segment",
              help="Vendor display name for the SoC JSON.")
@click.option("--display-name", default=None,
              show_default="derived from the SKU",
              help="Preset display_name.")
@click.option("--inference-backend", default=None,
              type=click.Choice(INFERENCE_BACKENDS),
              show_default="tbd",
              help="Silicon-determined inference backend "
                   "(`tbd` placeholder when unknown).")
@click.option("--ethos-u-variant", default=None,
              type=click.Choice(ETHOS_U_VARIANTS),
              help="Primary Ethos-U variant; required with "
                   "--inference-backend ethos_u.")
@click.option("--cores", default=None, callback=_split_csv,
              show_default=",".join(DEFAULT_CORES),
              help="Comma-separated canonical core ids.")
@click.option("--default-board", default=None, show_default=DEFAULT_BOARD,
              help="Stock carrier board (a `name:` from metadata/boards/).")
@click.option("--default-hw-rev", default=None, show_default=DEFAULT_HW_REV,
              help="Default hardware revision.")
@click.option("--output-root", default=None,
              type=click.Path(file_okay=False, path_type=Path),
              show_default="the SDK checkout",
              help="Root to generate metadata/ under.")
@click.option("--dry-run", is_flag=True,
              help="Validate and print the planned files; write nothing.")
@click.option("--force", is_flag=True,
              help="Overwrite an existing preset for this SKU.")
def new_som_cmd(
    sku: str | None,
    soc_ref: str | None,
    family: str | None,
    vendor: str | None,
    display_name: str | None,
    inference_backend: str | None,
    ethos_u_variant: str | None,
    cores: tuple[str, ...] | None,
    default_board: str | None,
    default_hw_rev: str | None,
    output_root: Path | None,
    dry_run: bool,
    force: bool,
) -> None:
    # -- 1. Gather inputs.  Interactive prompts need a real terminal; in
    # a pipe / CI, fail fast naming exactly what is missing instead of
    # letting questionary abort with an opaque "Aborted!".
    if sku is None or soc_ref is None or family is None:
        if not sys.stdin.isatty():
            missing = [flag for flag, value in (("--sku", sku),
                                                ("--soc-ref", soc_ref),
                                                ("--family", family))
                       if value is None]
            _fail("stdin is not a terminal, so interactive prompts are "
                  "unavailable; pass the missing required flag(s): "
                  + ", ".join(missing))
        (sku, soc_ref, family, vendor, display_name, inference_backend,
         ethos_u_variant, cores, default_board, default_hw_rev) = _interactive(
            sku, soc_ref, family, vendor, display_name, inference_backend,
            ethos_u_variant, cores, default_board, default_hw_rev,
        )

    # Flag-driven (CI) defaults for everything not provided.
    if inference_backend is None:
        inference_backend = "tbd"
    if cores is None:
        cores = DEFAULT_CORES
    if default_board is None:
        default_board = DEFAULT_BOARD
    if default_hw_rev is None:
        default_hw_rev = DEFAULT_HW_REV
    if output_root is None:
        output_root = REPO_ROOT

    # -- 2. Validate EVERYTHING before touching the filesystem, so a
    # rejected invocation never leaves half-written skeletons behind.
    if not _SKU_RE.match(sku):
        _fail(f"SKU '{sku}' must look like E1M-<UPPERCASE>")
    if not _SOC_REF_RE.match(soc_ref):
        _fail(f"soc-ref '{soc_ref}' must be "
              f"<vendor>:<family>:<part> (lowercase slugs)")
    if not _FAMILY_RE.match(family):
        _fail(f"family '{family}' must be a lowercase slug")
    if not cores:
        _fail("--cores was given but contains no core ids "
              "(omit the flag for the tbd_core0 placeholder)")
    bad_cores = [c for c in cores if not _CORE_ID_RE.match(c)]
    if bad_cores:
        _fail(f"core id(s) {bad_cores} must match ^[a-z][a-z0-9_]+$")
    if inference_backend == "ethos_u" and ethos_u_variant is None:
        _fail("--inference-backend ethos_u requires "
              "--ethos-u-variant (u55/u65/u85)")
    if vendor is None:
        vendor = soc_ref.split(":")[0]
    if display_name is None:
        display_name = f"{sku} ({vendor} -- scaffold, silicon facts TBD)"
    if any(ord(ch) < 0x20 for ch in display_name):
        _fail("display name must not contain newlines or other "
              "control characters")

    # Resolve the cross-references the checklist used to defer: the
    # stock carrier must exist in metadata/boards/, and the hw rev must
    # resolve in the family hw-revisions file when that file exists (a
    # brand-new family has none yet -- that stays a checklist step).
    board_names = _known_board_names()
    if board_names is not None and default_board not in board_names:
        _fail(f"default board '{default_board}' does not match any `name:` "
              f"in metadata/boards/ (known: {', '.join(sorted(board_names))})")
    hw_revs = _family_hw_revisions(sku, output_root)
    if hw_revs is not None:
        hwrev_path, revs = hw_revs
        if default_hw_rev not in revs:
            _fail(f"default hw rev '{default_hw_rev}' does not resolve in "
                  f"{hwrev_path} (known: {', '.join(sorted(revs))})")

    preset_path = output_root / "metadata" / "e1m_modules" / f"{sku}.yaml"
    vendor_slug, family_slug, part_slug = soc_ref.split(":")
    soc_path = (output_root / "metadata" / "socs" / vendor_slug
                / family_slug / f"{part_slug}.json")

    if preset_path.exists() and not force:
        _fail(f"{preset_path} already exists (pass --force to overwrite)")

    # -- 3. Render + self-check both skeletons in memory (still nothing
    # on disk): the skeletons must be schema-valid on arrival.  The one
    # sanctioned exception is a SKU outside the schema's current
    # pattern -- extending that pattern IS a porting step (see below).
    preset_text = _render_preset(
        sku, soc_ref, family, display_name, inference_backend,
        ethos_u_variant, cores, default_board, default_hw_rev,
    )
    soc_doc = None if soc_path.exists() else _soc_skeleton(
        sku, soc_ref, vendor, cores)

    sku_needs_pattern = re.match(_current_sku_pattern(), sku) is None
    try:
        import yaml
        preset_doc = yaml.safe_load(preset_text)
    except ImportError:
        preset_doc = None
    if preset_doc is not None:
        errors = _schema_errors(preset_doc, SOM_SCHEMA_PATH)
        if sku_needs_pattern:
            errors = [e for e in errors if not e.startswith("sku:")]
        if errors:
            click.echo("alp new-som: INTERNAL ERROR -- generated preset "
                       "does not validate against som-preset-v1:", err=True)
            for e in errors:
                click.echo(f"  - {e}", err=True)
            raise SystemExit(1)
        click.echo("Preset skeleton validates against som-preset-v1"
                   + (" (except the sku pattern -- see step below)"
                      if sku_needs_pattern else ""))
    if soc_doc is not None:
        errors = _schema_errors(soc_doc, SOC_SCHEMA_PATH)
        if errors:
            click.echo("alp new-som: INTERNAL ERROR -- generated SoC spec "
                       "does not validate against soc-spec-v1:", err=True)
            for e in errors:
                click.echo(f"  - {e}", err=True)
            raise SystemExit(1)
        click.echo("SoC spec skeleton validates against soc-spec-v1")

    # -- 4. Write (or, under --dry-run, only report) the skeletons.
    if dry_run:
        click.echo(f"Would create {preset_path}")
        if soc_doc is None:
            click.echo(f"SoC spec already present: {soc_path} (not touched)")
        else:
            click.echo(f"Would create {soc_path}")
    else:
        written: list[Path] = []
        try:
            preset_path.parent.mkdir(parents=True, exist_ok=True)
            preset_path.write_text(preset_text, encoding="utf-8", newline="\n")
            written.append(preset_path)
            if soc_doc is not None:
                soc_path.parent.mkdir(parents=True, exist_ok=True)
                soc_path.write_text(json.dumps(soc_doc, indent=2) + "\n",
                                    encoding="utf-8", newline="\n")
                written.append(soc_path)
        except OSError as exc:
            # Never leave a half-written scaffold behind: remove both
            # the partially-written target and anything already created.
            targets = [preset_path] + ([soc_path] if soc_doc is not None else [])
            for path in {*written, *targets}:
                path.unlink(missing_ok=True)
            _fail(f"could not write the skeletons ({exc}); "
                  f"removed any partial output")
        click.echo(f"Created {preset_path}")
        if soc_doc is None:
            click.echo(f"SoC spec already present: {soc_path} (not touched)")
        else:
            click.echo(f"Created {soc_path}")

    # -- 5. Numbered next-steps checklist.
    soc_created = soc_doc is not None
    steps = [
        f"Fill every TBD in {preset_path.name}"
        + (f" and {soc_path.name}" if soc_created else "")
        + " from the authoritative datasheet / schematic / BOM"
        " -- never invent values.",
    ]
    if sku_needs_pattern:
        steps.append(
            f"Extend the `sku:` pattern in "
            f"metadata/schemas/som-preset-v1.schema.json to accept {sku} "
            f"(docs/porting-new-som.md, schema-pattern step)."
        )
    steps.append(
        f"Register the silicon ref: add \"{soc_ref}\" to "
        f"metadata/registries/silicon-kconfig.json and the matching "
        f"ALP_SOC_* stanza in zephyr/Kconfig.")
    if hw_revs is None:
        # Only when the family hw-revisions file could not be resolved
        # at scaffold time (brand-new family); otherwise the rev was
        # already validated above.
        steps.append(
            f"Ensure default_hw_rev '{default_hw_rev}' resolves in "
            f"metadata/e1m_modules/<family-dir>/hw-revisions.yaml "
            f"(add the family file for a brand-new family).")
    steps += [
        "Validate all metadata: python scripts/validate_metadata.py",
        "Regenerate derived headers: python scripts/gen_soc_caps.py "
        "&& python scripts/gen_board_header.py",
        "Run the conformance suite: tests/zephyr/conformance via "
        "twister (native_sim).",
        "Full walkthrough: docs/porting-new-som.md",
    ]
    click.echo("")
    click.echo("Next steps:")
    for i, step in enumerate(steps, start=1):
        click.echo(f"  {i}. {step}")
    if dry_run:
        click.echo("")
        click.echo("Dry run -- validated OK, nothing was written.")
