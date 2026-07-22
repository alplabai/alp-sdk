#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic render / preview / validate engine for the templates
declared in metadata/templates/catalog-v1.json (schema:
metadata/schemas/template-catalog-v1.schema.json, drift gate:
scripts/check_template_catalog.py).

This is the epic #610 SS3 follow-up: the catalog only *describes* each
template (id, canonical example, user-owned vs SDK-generated files,
parameters, test wiring); this module actually materialises one --

  * render(template_id, dest_dir, params) -- copy every
    files.user_owned path from the catalog record's canonical
    `example` directory into dest_dir, byte-for-byte, in sorted
    traversal order, never touching files.generated (those are
    emitted later by scripts/alp_project.py at configure time, per
    the catalog's own `generated_artifacts` note). Same inputs
    produce byte-identical output every time: no timestamps, no
    filesystem-metadata copy (shutil.copyfile-style, not copy2), no
    non-deterministic ordering.

  * render(..., dry_run=True) / the `--dry-run` CLI flag -- the exact
    same planning path, minus the disk writes: a preview of what
    render() would do.

  * render_to_envelope(template_id, sku) -- the in-memory capture path
    behind `scripts/alp_project.py --emit scaffold --template <id>
    --sku <SKU>` (issue #864, deepened by its own follow-up): renders
    entirely in memory (no dest_dir, no disk write) and returns
    `[(path, contents), ...]` in the same sorted order render() writes,
    sharing the same per-file read/substitute loop so the two can
    never disagree on a byte for the files that stay a faithful copy.
    `sku` must be one of the template's declared `supported.som_skus`
    (SkuNotSupportedError otherwise, naming the supported set); the
    rendered `board.yaml`'s `som.sku:` and top-level `preset:` lines are
    substituted for `sku`'s own default board (metadata/e1m_modules/
    <sku>.yaml `default_board:`). The app CORE is re-derived too: a
    template's `cores:`/`--core` id is the CANONICAL example's own SoM
    core (e.g. `m55_hp`, an Alif-only Zephyr cluster) -- wrong, and a
    non-buildable scaffold, for any `sku` whose topology doesn't have
    that core (every cross-SoM-family sku the catalog declares, e.g.
    E1M-V2N101 for the AEN-canonical `minimal` template). `board.yaml`'s
    `cores:` key and CMakeLists.txt's `--core` flag are re-derived from
    `sku`'s own `metadata/e1m_modules/<sku>.yaml` `topology:` (see
    `_derive_core_renames`) whenever the canonical core isn't already
    valid for `sku`; a no-op (byte-identical) when it already is -- the
    canonical example's own sku, or a same-family sibling. CMakeLists.txt
    and README.md are ALSO scaffold-flavoured regardless of `sku` (see
    `_scaffold_cmakelists` / `_scaffold_readme`): the in-tree
    `ALP_SDK_ROOT` guess and the SDK-tree-relative links/paths those
    files carry are wrong for a scaffold copied out of the SDK tree,
    sku or no sku -- render()/validate() stay byte-for-byte faithful
    (that's what validate()'s twister run proves builds); only
    render_to_envelope() scaffold-adapts. `testcase.yaml` is not part
    of the envelope (dropped from the catalog's `files.user_owned`: SDK
    CI wiring, not a user's project file); validate() copies it
    separately, straight from the catalog's `test.testcase_yaml`, for
    its own internal twister self-test. This is the SDK-owned single
    source of the scaffold's build-integration conventions -- the
    surface tan-cli vendors at release instead of hand-porting a
    per-SKU Rust generator.

  * validate(template_id) -- renders the template into a fresh
    tempfile.mkdtemp() directory and runs its native_sim scenario(s)
    there via the real Zephyr twister (mirroring
    scripts/test-all.sh's stage_twister invocation), asserting at
    least one test actually passed, then deletes the temp directory.
    This is the epic's "every generated project must be validated in
    a temporary directory before publication" requirement. Skips
    cleanly (does not fail) when ZEPHYR_BASE is unset, matching every
    other twister-needing path in this repo.

Parameter substitution is deliberately inert for every template the
catalog ships today: metadata/schemas/template-catalog-v1.schema.json's
`parameter` definition is `additionalProperties: false` (name / type /
description / default / constraints only) -- no shipped parameter can
declare WHERE in its example's files it applies. render() validates any
`--param name=value` override against the declared parameter's type and
`constraints` (so a bogus name or an out-of-enum/out-of-range value is a
hard error), but performs no textual substitution unless the parameter
carries an opt-in `substitute: {"file": ..., "literal": ...}` mapping --
a library-level extension the shipped schema doesn't emit yet. That
keeps `minimal` (and every other template today) a pure faithful copy,
per #610 SS3's requirement to keep substitution "minimal + honest".
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_template: PyYAML is required.  Install via `pip install pyyaml`.")

from alp_project_loader import METADATA_ROOT

REPO = Path(__file__).resolve().parent.parent
CATALOG = REPO / "metadata" / "templates" / "catalog-v1.json"


class TemplateError(Exception):
    """Base for every error alp_template raises -- never a bare
    KeyError/ValueError escapes to a caller."""


class TemplateNotFoundError(TemplateError):
    """No record with this id in the catalog."""


class DestinationNotEmptyError(TemplateError):
    """dest_dir exists, is non-empty, and force was not requested."""


class ParameterError(TemplateError):
    """An unknown parameter name, a type mismatch, or a constraint
    violation (enum / minimum / maximum)."""


class SkuNotSupportedError(TemplateError):
    """`render_to_envelope`'s --sku is not in the record's declared
    `supported.som_skus` -- a hard error, never a best-effort render."""


@dataclasses.dataclass(frozen=True)
class RenderPlan:
    """What render() would do -- returned for BOTH the dry-run preview
    and the real render, so a caller can diff "planned" vs "done"."""

    template_id: str
    dest_dir: Path
    files: tuple[str, ...]  # files.user_owned paths, sorted
    substitutions: tuple[tuple[str, str, str], ...]  # (param, default, value)


@dataclasses.dataclass(frozen=True)
class ValidateResult:
    """Outcome of validate(). `skipped` is truthy exactly when
    ZEPHYR_BASE was unset -- callers should treat that as a clean
    no-op, not a failure, matching every other twister-needing path in
    this repo (scripts/test-all.sh's stage_twister, etc.)."""

    template_id: str
    skipped: bool
    reason: str = ""
    passed: bool = False
    returncode: int | None = None
    passed_count: int = 0
    tmp_dir: str | None = None
    stdout: str = ""
    stderr: str = ""


def load_catalog(catalog_path: Path | None = None) -> dict[str, Any]:
    path = catalog_path or CATALOG
    return json.loads(path.read_text(encoding="utf-8"))


def find_template(doc: dict[str, Any], template_id: str) -> dict[str, Any]:
    for rec in doc.get("templates", []):
        if rec["id"] == template_id:
            return rec
    known = ", ".join(sorted(t["id"] for t in doc.get("templates", [])))
    raise TemplateNotFoundError(
        f"no template {template_id!r} in catalog (known: {known})")


def _coerce(spec: dict[str, Any], raw: Any) -> Any:
    """Coerce a CLI-style string override to the parameter's declared
    type. Values already of the right type (e.g. an untouched default,
    or a native value a Python caller passed directly) pass through."""
    if not isinstance(raw, str):
        return raw
    ptype = spec["type"]
    if ptype == "integer":
        try:
            return int(raw)
        except ValueError as exc:
            raise ParameterError(
                f"{spec['name']}: {raw!r} is not an integer") from exc
    if ptype == "boolean":
        if raw.lower() in ("1", "true", "yes"):
            return True
        if raw.lower() in ("0", "false", "no"):
            return False
        raise ParameterError(f"{spec['name']}: {raw!r} is not a boolean")
    return raw  # string / enum stay strings


def _check_constraints(template_id: str, spec: dict[str, Any], value: Any) -> None:
    constraints = spec.get("constraints") or {}
    if "enum" in constraints and value not in constraints["enum"]:
        raise ParameterError(
            f"{template_id}: {spec['name']}={value!r} not in "
            f"{constraints['enum']}")
    if "minimum" in constraints and value < constraints["minimum"]:
        raise ParameterError(
            f"{template_id}: {spec['name']}={value!r} < minimum "
            f"{constraints['minimum']}")
    if "maximum" in constraints and value > constraints["maximum"]:
        raise ParameterError(
            f"{template_id}: {spec['name']}={value!r} > maximum "
            f"{constraints['maximum']}")


def _resolve_params(
    record: dict[str, Any], params: dict[str, Any] | None,
) -> dict[str, Any]:
    """Resolve every declared parameter to its effective value (override
    or default), rejecting any name the record doesn't declare -- this
    can never invent a knob the catalog doesn't have."""
    declared = {p["name"]: p for p in record.get("parameters", [])}
    params = dict(params or {})
    unknown = sorted(set(params) - set(declared))
    if unknown:
        raise ParameterError(
            f"{record['id']}: unknown parameter(s) {unknown}; declared: "
            f"{sorted(declared) or '(none)'}")

    resolved: dict[str, Any] = {}
    for name, spec in declared.items():
        value = _coerce(spec, params.get(name, spec["default"]))
        _check_constraints(record["id"], spec, value)
        resolved[name] = value
    return resolved


def _substitutions_for(
    record: dict[str, Any], resolved: dict[str, Any],
) -> dict[str, list[tuple[str, str]]]:
    """dest-relative file -> [(literal_to_replace, new_value_str), ...].

    Reads an opt-in `substitute: {"file": ..., "literal": <optional,
    defaults to str(default)>}` key on a parameter record. No parameter
    the shipped catalog declares today carries this key (the schema
    forbids it -- additionalProperties: false), so this is a no-op for
    every real template; see the module docstring and
    tests/scripts/test_alp_template.py's synthetic-fixture case.
    """
    per_file: dict[str, list[tuple[str, str]]] = {}
    for spec in record.get("parameters", []):
        sub = spec.get("substitute")
        if not sub:
            continue
        value = resolved[spec["name"]]
        if value == spec["default"]:
            continue  # override equals default: nothing to change
        literal = sub.get("literal", str(spec["default"]))
        per_file.setdefault(sub["file"], []).append((literal, str(value)))
    return per_file


def plan(
    template_id: str,
    dest_dir: str | os.PathLike[str],
    params: dict[str, Any] | None = None,
    *,
    catalog_path: Path | None = None,
) -> tuple[dict[str, Any], RenderPlan]:
    """Compute (and validate) what render() would do, without touching
    disk. Shared by render()'s dry-run path and its real path so preview
    and render can never disagree."""
    doc = load_catalog(catalog_path)
    record = find_template(doc, template_id)
    resolved = _resolve_params(record, params)
    files = tuple(sorted(record["files"]["user_owned"]))
    subs: list[tuple[str, str, str]] = []
    for spec in record.get("parameters", []):
        value = resolved[spec["name"]]
        if value != spec["default"]:
            subs.append((spec["name"], str(spec["default"]), str(value)))
    render_plan = RenderPlan(
        template_id=template_id,
        dest_dir=Path(dest_dir),
        files=files,
        substitutions=tuple(subs),
    )
    return record, render_plan


def _rendered_bytes(
    template_id: str,
    record: dict[str, Any],
    files: tuple[str, ...],
    resolved: dict[str, Any],
    base_dir: Path,
) -> list[tuple[str, bytes]]:
    """Read + apply every declared-parameter substitution for `files`
    (a RenderPlan.files list), returning [(relpath, bytes), ...] in the
    same order. Shared by render()'s disk-write loop and
    render_to_envelope()'s in-memory capture -- the same bytes a
    customer gets from `alp_template.py render` are what `--emit
    scaffold` hands back as JSON `contents` (see the module docstring)."""
    example = base_dir / record["example"]
    file_subs = _substitutions_for(record, resolved)
    out: list[tuple[str, bytes]] = []
    for rel in files:
        data = (example / rel).read_bytes()
        subs = file_subs.get(rel)
        if subs:
            text = data.decode("utf-8")
            for literal, value in subs:
                if literal not in text:
                    raise ParameterError(
                        f"{template_id}: substitution literal {literal!r} "
                        f"not found in {rel}")
                text = text.replace(literal, value)
            data = text.encode("utf-8")
        out.append((rel, data))
    return out


def render(
    template_id: str,
    dest_dir: str | os.PathLike[str],
    params: dict[str, Any] | None = None,
    *,
    dry_run: bool = False,
    force: bool = False,
    catalog_path: Path | None = None,
    base_dir: Path | None = None,
) -> RenderPlan:
    """Materialise `template_id` into `dest_dir`.

    Copies every `files.user_owned` path from the catalog record's
    `example` directory into dest_dir, preserving the relative layout,
    byte-for-byte (shutil.copyfile-equivalent -- no filesystem metadata,
    no timestamp embedded in content). `files.generated` paths are never
    copied -- those are emitted later, at build-configure time, by
    scripts/alp_project.py.

    Deterministic: given the same template_id/params, the file list and
    byte content written are identical on every call (sorted traversal,
    no wall-clock/host-identity content). Refuses to touch a dest_dir
    that already exists and is non-empty unless force=True.

    dry_run=True (the CLI's --dry-run) returns the exact same RenderPlan
    a real render would produce, without writing anything.

    `base_dir` (default: the alp-sdk repo root) is the directory the
    catalog record's `example` path is resolved against. Production
    callers never need it; it exists so tests can point at a hermetic
    fixture tree without writing into the real repo (see
    tests/scripts/test_alp_template.py's synthetic-fixture case).
    """
    record, render_plan = plan(template_id, dest_dir, params, catalog_path=catalog_path)
    if dry_run:
        return render_plan

    dest = Path(dest_dir)
    if dest.exists():
        if not dest.is_dir():
            raise DestinationNotEmptyError(f"{dest} exists and is not a directory")
        if any(dest.iterdir()) and not force:
            raise DestinationNotEmptyError(
                f"{dest} is not empty (pass force=True / --force to overwrite)")

    resolved = _resolve_params(record, params)
    base = base_dir or REPO

    dest.mkdir(parents=True, exist_ok=True)
    for rel, data in _rendered_bytes(template_id, record, render_plan.files, resolved, base):
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    return render_plan


# ---------------------------------------------------------------------
# --emit scaffold (issue #864): in-memory, SKU-parameterised capture
# ---------------------------------------------------------------------

def _load_som_doc(sku: str, metadata_root: Path) -> dict[str, Any]:
    """Parse metadata/e1m_modules/<sku>.yaml -- shared by
    `_default_preset_for_sku` (the `default_board:` field) and
    `_derive_core_renames` (the `topology:` block), so both read the
    exact same doc for the same `(sku, metadata_root)`."""
    som_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not som_path.is_file():
        raise TemplateError(
            f"no metadata/e1m_modules/{sku}.yaml for sku {sku!r}")
    return yaml.safe_load(som_path.read_text(encoding="utf-8")) or {}


def _default_preset_for_sku(sku: str, metadata_root: Path) -> str:
    """The board preset a fresh project targeting `sku` ships with by
    default -- metadata/e1m_modules/<sku>.yaml's `default_board:`,
    lower-cased to match the `preset:` value every example board.yaml
    already uses (e.g. `E1M-EVK` -> `e1m-evk`, `E1M-X-EVK` ->
    `e1m-x-evk`). This is the SAME field board.yaml's own comments point
    customers at by hand ("copy this directory, change som.sku ...,
    edit the preset:") -- render_to_envelope just does that edit for
    them."""
    board = _load_som_doc(sku, metadata_root).get("default_board")
    if not board:
        raise TemplateError(
            f"metadata/e1m_modules/{sku}.yaml has no default_board")
    return board.lower()


def _core_ids_from_board_yaml(text: str) -> list[str]:
    """The `cores:` mapping keys a rendered board.yaml declares -- a
    real YAML parse (not a line regex), since core re-derivation needs
    the exact declared set, not a single line's value."""
    doc = yaml.safe_load(text) or {}
    return list((doc.get("cores") or {}).keys())


def _derive_core_renames(
    original_core_ids: list[str], sku: str, metadata_root: Path,
) -> dict[str, str] | None:
    """Re-derive every STALE core id a catalog template's `cores:`
    block declares, for `sku`'s OWN SoM topology (issue #864 follow-up:
    the shallow "byte-copy the example + swap som.sku" `render_to_
    envelope()` #864 shipped hard-coded the CANONICAL example's own
    core id -- e.g. `m55_hp`, an Alif-only Zephyr cluster -- into every
    substituted board.yaml/CMakeLists.txt, emitting a non-buildable
    scaffold for any cross-SoM-family sku: `alp_project.py --emit
    zephyr-conf --core m55_hp` against an E1M-V2N101 board.yaml fails
    with rc=1, "unknown core id ... did you mean ['a55_cluster',
    'm33_sm']").

    EVERY key `cores:` declares must exist in the target sku's own
    topology -- `alp_orchestrate.loader._validate_topology_cores` hard-
    errors on an unmatched key unconditionally, whether or not that
    core is `os: off` -- so a template that also declares the OTHER
    cluster explicitly disabled (edge-ai's `cores.a32_cluster: {os:
    off}`, alongside the active `m55_hp`) needs THAT id renamed too,
    not just the one core the app actually runs on.

    Returns `None` when every declared id already exists in `sku`'s
    `metadata/e1m_modules/<sku>.yaml` `topology:` -- the canonical
    example's own SoM, or a same-family sibling that shares its core
    ids -- a byte-identical passthrough, nothing to rewrite. Otherwise
    returns `{old_core_id: new_core_id, ...}` for every stale id: each
    replacement is `sku`'s own topology core sharing the same leading
    core-class letter (`m`/`a` -- the SDK-wide one-letter-prefix
    convention `alp_project_emit.hw_info._pick_primary_core_os` also
    keys off), additionally requiring a Zephyr `board:` target for an
    `m`-class replacement (only that core is ever `--core`-buildable,
    which is why CMakeLists.txt needs it too -- see
    `_substitute_cmake_core`); an `a`-class utility core carries no
    such requirement (it's only ever `os: off` in every template that
    declares one today).
    """
    topology = _load_som_doc(sku, metadata_root).get("topology") or {}
    stale = [cid for cid in original_core_ids if cid not in topology]
    if not stale:
        return None
    claimed = set(original_core_ids) & set(topology)
    renames: dict[str, str] = {}
    for old in stale:
        prefix = old[0]
        require_board = prefix == "m"
        candidates = sorted(
            cid for cid, spec in topology.items()
            if cid.startswith(prefix) and cid not in claimed
            and cid not in renames.values()
            and (spec.get("board") if require_board else True))
        if not candidates:
            raise TemplateError(
                f"metadata/e1m_modules/{sku}.yaml topology has no "
                f"{prefix!r}-class core"
                + (" with a Zephyr `board:` target" if require_board else "")
                + f" to replace {old!r}")
        renames[old] = candidates[0]
    return renames


# Matches board.yaml's `som:\n  sku: E1M-...` line and the top-level
# `preset: <name>` line -- through end-of-line (incl. any trailing inline
# comment), so a value CHANGE can drop a comment describing the OLD SoM
# (e.g. `sku: E1M-AEN801   # Alif Ensemble E8 SoM` must not survive as a
# stale label once the value becomes E1M-V2N101). Unbounded (no count=):
# every match is inspected so a board.yaml with more than one matching
# `sku:`/`preset:` line -- ambiguous, could silently rewrite a decoy while
# the real som.sku/preset line survives untouched -- hard-errors instead
# of guessing which one is real.
_SOM_SKU_RE = re.compile(r"(?m)^(\s*sku:\s*)(E1M-[A-Z0-9]+)[^\n]*$")
_PRESET_RE = re.compile(r"(?m)^(preset:\s*)(\S+)[^\n]*$")


def _substitute_board_yaml_sku(text: str, sku: str, preset: str) -> str:
    def _sub_sku(m: re.Match[str]) -> str:
        # Value unchanged -> leave the WHOLE line (incl. any comment)
        # untouched: this is the byte-passthrough guarantee for sku ==
        # the example's own default.
        return m.group(0) if m.group(2) == sku else f"{m.group(1)}{sku}"

    text, n_sku = _SOM_SKU_RE.subn(_sub_sku, text)
    if n_sku != 1:
        raise TemplateError(
            f"board.yaml must have exactly one `som.sku:` line to "
            f"substitute (found {n_sku})")

    def _sub_preset(m: re.Match[str]) -> str:
        return m.group(0) if m.group(2) == preset else f"{m.group(1)}{preset}"

    text, n_preset = _PRESET_RE.subn(_sub_preset, text)
    if n_preset != 1:
        raise TemplateError(
            f"board.yaml must have exactly one top-level `preset:` line "
            f"to substitute (found {n_preset})")
    return text


_LIBRARY_CORE_SCOPE_RE = re.compile(r"(cores:\s*\[)([^\]]*)(\])")


def _substitute_board_yaml_core(text: str, old: str, new: str) -> str:
    """Rewrite the `cores:` mapping's single top-level `<old>:` key to
    `<new>:`. The per-core content underneath (`app:`, `peripherals:`)
    is core-id-agnostic -- metadata/schemas/board.schema.json's
    `core_entry` says every field is optional and inherits the SoM
    preset's `topology.<core_id>` default, so only the KEY changes.

    Also renames `old` wherever a top-level `libraries:` entry scopes
    itself to this core via a `cores: [<id>, ...]` flow list (e.g.
    cold-chain-monitor's `libraries: [{name: tflite-micro, cores:
    [m55_hp]}]`) -- `alp_orchestrate.loader._normalize_libraries` hard-
    errors if that list still names a core id that no longer exists
    once the `cores:` mapping key above is renamed ("libraries: entry
    '<name>' is scoped to core '<old>', which is not declared under
    `cores:`")."""
    pattern = re.compile(rf"(?m)^(\s*){re.escape(old)}:([ \t]*)$")
    new_text, n = pattern.subn(lambda m: f"{m.group(1)}{new}:{m.group(2)}", text)
    if n != 1:
        raise TemplateError(
            f"board.yaml must have exactly one `cores.{old}:` line to "
            f"re-derive to {new!r} (found {n})")

    def _fix_scope_list(m: re.Match[str]) -> str:
        inner = re.sub(rf"\b{re.escape(old)}\b", new, m.group(2))
        return f"{m.group(1)}{inner}{m.group(3)}"

    return _LIBRARY_CORE_SCOPE_RE.sub(_fix_scope_list, new_text)


def _substitute_cmake_core(text: str, old: str, new: str) -> str:
    """Rewrite CMakeLists.txt's `alp_project.py --emit zephyr-conf
    --core <old>` invocation to the re-derived core id."""
    pattern = re.compile(rf"(--core\s+){re.escape(old)}\b")
    new_text, n = pattern.subn(lambda m: f"{m.group(1)}{new}", text)
    if n != 1:
        raise TemplateError(
            f"CMakeLists.txt must have exactly one `--core {old}` to "
            f"re-derive to {new!r} (found {n})")
    return new_text


# ---------------------------------------------------------------------
# --emit scaffold content adaptation (issue #864 follow-up)
# ---------------------------------------------------------------------
#
# Every catalog template's user_owned files are the SDK's own example,
# verbatim -- correct for render()'s documented byte-for-byte contract
# (validate()'s in-tree twister self-test relies on exactly that), but
# wrong for a scaffold a customer unpacks OUTSIDE the SDK tree: a
# `west build ... examples/<...>` argument naming a path that doesn't
# exist in their project, `../`-relative links that only resolve
# inside the SDK checkout, and a CMakeLists.txt that silently guesses
# `../../..` for ALP_SDK_ROOT (correct only for the in-tree example,
# never a copied-out scaffold -- the retired tan-cli generator hard-
# failed on exactly this: "ALP_SDK_ROOT is not set"). These transforms
# run ONLY in render_to_envelope() (the `--emit scaffold` path, for
# EVERY sku including the canonical example's own) -- render()/
# validate() stay byte-for-byte faithful to the real example, since
# that's what validate()'s temp-dir twister run is proving builds.

_ALP_SDK_ROOT_GUESS_RE = re.compile(
    r"if\(DEFINED ENV\{ALP_SDK_ROOT\}\)\n"
    r"    set\(ALP_SDK_ROOT \$ENV\{ALP_SDK_ROOT\}\)\n"
    r"else\(\)\n"
    r"    get_filename_component\(ALP_SDK_ROOT \$\{CMAKE_CURRENT_SOURCE_DIR\}(?:/\.\.)+ ABSOLUTE\)\n"
    r"endif\(\)"
)
_HARDCODED_ALP_PROJECT_PY_RE = re.compile(
    r"\$\{CMAKE_CURRENT_SOURCE_DIR\}(?:/\.\.)+/scripts/alp_project\.py"
)
_ALP_SDK_ROOT_REQUIRED_BLOCK = (
    "if(NOT DEFINED ENV{ALP_SDK_ROOT})\n"
    "    message(FATAL_ERROR\n"
    "        \"ALP_SDK_ROOT is not set -- point it at your alp-sdk checkout, \"\n"
    "        \"e.g. `export ALP_SDK_ROOT=/path/to/alp-sdk` (or "
    "-DALP_SDK_ROOT=... on the cmake command line).\")\n"
    "endif()\n"
    "set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})"
)


def _scaffold_cmakelists(text: str) -> str:
    """Replace an in-tree-relative ALP_SDK_ROOT guess with a hard
    requirement. Two shapes exist across the catalog's example
    CMakeLists.txt files today: the `if(DEFINED ENV{...}) ... else()
    get_filename_component(...)` guess (most examples), and
    `cold-chain-monitor`'s hardcoded `${CMAKE_CURRENT_SOURCE_DIR}/../..
    /../scripts/alp_project.py` call with no ALP_SDK_ROOT resolution at
    all (worse: no override is even possible). Best-effort: a
    CMakeLists.txt matching neither shape (e.g. multicore-rpmsg's
    linux/CMakeLists.txt, which never invokes alp_project.py) is
    returned unchanged."""
    new_text, n = _ALP_SDK_ROOT_GUESS_RE.subn(_ALP_SDK_ROOT_REQUIRED_BLOCK, text)
    if n:
        return new_text
    if _HARDCODED_ALP_PROJECT_PY_RE.search(text):
        text = _HARDCODED_ALP_PROJECT_PY_RE.sub(
            "${ALP_SDK_ROOT}/scripts/alp_project.py", text)
        return text.replace(
            "execute_process(\n",
            _ALP_SDK_ROOT_REQUIRED_BLOCK + "\n\nexecute_process(\n", 1)
    return text


_RELATIVE_LINK_RE = re.compile(r"\]\((\.\./[^)\s]+)\)")


def _scaffold_readme(text: str, example_path: str) -> str:
    """Every vendored README's `../`-relative links (`../../../docs/
    x.md`, a sibling example's `../i2c-scanner/`, ...) resolve against
    the CANONICAL example's OWN position inside the alp-sdk tree --
    dangling once copied out as a standalone scaffold. Rewrite each to
    an absolute GitHub URL instead. Also rewrites the one non-existent-
    once-copied-out token every Build section carries: a `west build
    ...` invocation naming THIS template's own repo-relative example
    path -- the scaffold IS the project root wherever the customer
    unpacks it, so that argument becomes `.`. Best-effort (neither
    pattern found -> text returned unchanged); per-template narrative
    prose (e.g. `tan build alp-sdk/examples/...` invocations, cross-
    references phrased as prose rather than a link) is intentionally
    not scaffold-normalised by this pass."""
    def _fix_link(m: re.Match[str]) -> str:
        target = posixpath.normpath(f"{example_path}/{m.group(1)}")
        kind = "blob" if "." in target.rsplit("/", 1)[-1] else "tree"
        return f"](https://github.com/alplabai/alp-sdk/{kind}/main/{target})"

    text = _RELATIVE_LINK_RE.sub(_fix_link, text)
    text = re.sub(rf"(?<!\S){re.escape(example_path)}(?!\S)", ".", text)
    return text


def render_to_envelope(
    template_id: str,
    sku: str,
    params: dict[str, Any] | None = None,
    *,
    catalog_path: Path | None = None,
    base_dir: Path | None = None,
    metadata_root: Path | None = None,
) -> list[tuple[str, str]]:
    """Render `template_id` for `sku` entirely in memory: no dest_dir, no
    disk write. Returns `[(path, contents), ...]` in the same sorted
    order `plan()` computes -- the `{path, contents}[]` shape
    `scripts/alp_project.py --emit scaffold` JSON-encodes (issue #864),
    matching the shape `--emit build-plan`'s `configArtefacts` /
    `sharedArtefacts` already use. `testcase.yaml` is never in this
    envelope (dropped from the catalog's `files.user_owned`: SDK CI
    wiring, not a user's project file).

    `sku` MUST be one of the record's declared `supported.som_skus` --
    SkuNotSupportedError (naming the supported set) otherwise, never a
    silent best-effort render. The rendered `board.yaml`'s `som.sku:`
    and top-level `preset:` are substituted for `sku`'s own default
    board (metadata/e1m_modules/<sku>.yaml `default_board:`). The app
    CORE is re-derived too (`_derive_core_renames`): `board.yaml`'s
    `cores:` key(s) and CMakeLists.txt's `--core` flag are rewritten
    from the canonical example's own SoM core (e.g. `m55_hp`) to
    `sku`'s own Zephyr-buildable core (e.g. `m33_sm` for E1M-V2N101)
    whenever the canonical core isn't already valid for `sku` -- this
    is the fix for issue #864's follow-up: the shallow `som.sku`-only
    swap emitted a board.yaml `--emit zephyr-conf --core m55_hp` can't
    build against for any cross-SoM-family sku. `board.yaml`/`prj.conf`
    /`src/main.c` are a byte-identical passthrough when `sku` already
    matches the example's own default (or shares its core ids);
    CMakeLists.txt and README.md are ALSO scaffold-adapted regardless
    of `sku` (`_scaffold_cmakelists` / `_scaffold_readme`) -- their
    in-tree `ALP_SDK_ROOT` guess and SDK-tree-relative links/paths are
    wrong for a copied-out scaffold no matter which sku was requested.
    """
    doc = load_catalog(catalog_path)
    record = find_template(doc, template_id)
    supported = record["supported"]["som_skus"]
    if sku not in supported:
        raise SkuNotSupportedError(
            f"{template_id}: sku {sku!r} is not supported "
            f"(supported: {sorted(supported)})")

    _, render_plan = plan(template_id, ".", params, catalog_path=catalog_path)
    resolved = _resolve_params(record, params)
    base = base_dir or REPO
    metadata_root = metadata_root or METADATA_ROOT
    preset = _default_preset_for_sku(sku, metadata_root)

    board_yaml_path = base / record["example"] / "board.yaml"
    original_core_ids = _core_ids_from_board_yaml(
        board_yaml_path.read_text(encoding="utf-8"))
    core_renames = _derive_core_renames(original_core_ids, sku, metadata_root)
    # The one rename CMakeLists.txt's `--core` flag also needs (the
    # m-class core the app actually builds on) -- None when nothing
    # was renamed, or when the template has no m-class core at all
    # (never happens for a real `runtimes: [zephyr]` catalog record).
    app_core_sub = next(
        ((old, new) for old, new in (core_renames or {}).items()
         if old.startswith("m")), None)

    out: list[tuple[str, str]] = []
    for rel, data in _rendered_bytes(template_id, record, render_plan.files, resolved, base):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as exc:
            # render() copies any file as raw bytes; the JSON envelope
            # cannot -- a future binary user_owned asset must fail
            # cleanly here, not escape _run_scaffold_emit's `except
            # TemplateError` as a raw traceback.
            raise TemplateError(
                f"{template_id}: {rel} is not valid UTF-8 text, cannot "
                f"be JSON-encoded for --emit scaffold ({exc})") from exc
        if rel == "board.yaml":
            text = _substitute_board_yaml_sku(text, sku, preset)
            for old, new in (core_renames or {}).items():
                text = _substitute_board_yaml_core(text, old, new)
        elif rel.endswith("CMakeLists.txt"):
            if app_core_sub:
                text = _substitute_cmake_core(text, *app_core_sub)
            text = _scaffold_cmakelists(text)
        elif rel == "README.md":
            text = _scaffold_readme(text, record["example"])
        out.append((rel, text))
    return out


def _count_passed(outdir: Path) -> int:
    """Count `status: passed` testcases in twister's own JSON report --
    the authoritative source, so "returncode 0" alone (which twister also
    returns when --testsuite-root matches zero tests) can't be mistaken
    for "at least one test actually passed"."""
    report = outdir / "twister.json"
    if not report.is_file():
        return 0
    try:
        data = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return 0
    count = 0
    for suite in data.get("testsuites", []):
        cases = suite.get("testcases", [])
        if cases:
            count += sum(1 for tc in cases if tc.get("status") == "passed")
        elif suite.get("status") == "passed":
            # Some twister versions/harnesses report status only at the
            # suite level with no per-testcase breakdown.
            count += 1
    return count


def validate(
    template_id: str,
    *,
    catalog_path: Path | None = None,
    zephyr_base: str | None = None,
    keep_tmp: bool = False,
) -> ValidateResult:
    """Render `template_id` into a fresh temp directory and run its
    native_sim scenario(s) there via the real Zephyr twister, asserting
    at least one test passed -- the epic #610 SS3 "validated in a
    temporary directory before publication" gate. Cleans the temp
    directory up afterwards (unless keep_tmp, for debugging).

    Skips cleanly (skipped=True) when ZEPHYR_BASE is not set, matching
    every other twister-needing path in this repo (see
    scripts/test-all.sh's stage_twister).
    """
    if zephyr_base is None:
        zephyr_base = os.environ.get("ZEPHYR_BASE", "").strip()
    if not zephyr_base:
        return ValidateResult(
            template_id=template_id, skipped=True, reason="ZEPHYR_BASE not set")

    tmp = Path(tempfile.mkdtemp(prefix=f"alp-template-{template_id}-"))
    try:
        render(template_id, tmp, force=True, catalog_path=catalog_path)

        # `testcase.yaml` isn't part of `files.user_owned` (issue #864
        # follow-up -- a customer's scaffold doesn't ship the SDK's own
        # twister case), so render() above doesn't copy it; this gate's
        # "run the real native_sim scenario" self-test still needs one
        # for twister to discover a testsuite. Copy it here directly
        # from the catalog's `test.testcase_yaml` list instead -- purely
        # internal to validate(), never part of what a customer's
        # scaffold receives.
        doc = load_catalog(catalog_path)
        record = find_template(doc, template_id)
        example_prefix = record["example"] + "/"
        for tc in record["test"]["testcase_yaml"]:
            rel = (tc[len(example_prefix):] if tc.startswith(example_prefix)
                    else Path(tc).name)
            dst = tmp / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes((REPO / tc).read_bytes())

        outdir = tmp / "twister-out"
        env = os.environ.copy()
        env["ZEPHYR_BASE"] = zephyr_base
        # CMakeLists.txt resolves ALP_SDK_ROOT from this env var when set
        # (see examples/*/CMakeLists.txt) -- required here since the temp
        # dir isn't three levels under an alp-sdk checkout, so the
        # in-tree relative fallback (../../..) would resolve to garbage.
        env["ALP_SDK_ROOT"] = str(REPO)
        # EXTRA_ZEPHYR_MODULES is ';'-separated (Zephyr's own convention,
        # not os.pathsep) -- mirrors scripts/test-all.sh's stage_twister,
        # which pins REPO_ROOT as the alp-sdk Zephyr module the same way.
        existing = [
            m for m in env.get("EXTRA_ZEPHYR_MODULES", "").split(";")
            if m and m != str(REPO)
        ]
        existing.append(str(REPO))
        env["EXTRA_ZEPHYR_MODULES"] = ";".join(existing)

        cmd = [
            sys.executable,
            str(Path(zephyr_base) / "scripts" / "twister"),
            "--testsuite-root", str(tmp),
            "-p", "native_sim/native/64",
            "--inline-logs",
            "--no-detailed-test-id",
            "--outdir", str(outdir),
        ]
        proc = subprocess.run(
            cmd, cwd=tmp, env=env, capture_output=True, text=True, check=False)
        passed_count = _count_passed(outdir)
        return ValidateResult(
            template_id=template_id,
            skipped=False,
            passed=(proc.returncode == 0 and passed_count >= 1),
            returncode=proc.returncode,
            passed_count=passed_count,
            tmp_dir=str(tmp),
            stdout=proc.stdout,
            stderr=proc.stderr,
        )
    finally:
        if not keep_tmp:
            shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _parse_param(raw: str) -> tuple[str, str]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError(f"--param expects name=value, got {raw!r}")
    name, _, value = raw.partition("=")
    return name, value


def _cli_render(args: argparse.Namespace) -> int:
    params = dict(args.param or [])
    try:
        result = render(
            args.template_id, args.dest, params,
            dry_run=args.dry_run, force=args.force, catalog_path=args.catalog)
    except TemplateError as exc:
        print(f"alp_template: {exc}", file=sys.stderr)
        return 1

    verb = "would write" if args.dry_run else "wrote"
    dest = Path(args.dest)
    for rel in result.files:
        print(f"{verb} {dest / rel}")
    for name, default, value in result.substitutions:
        print(f"  substituting {name}: {default!r} -> {value!r}")
    return 0


def _cli_validate(args: argparse.Namespace) -> int:
    result = validate(
        args.template_id, catalog_path=args.catalog,
        zephyr_base=args.zephyr_base, keep_tmp=args.keep_tmp)
    if result.skipped:
        print(f"SKIP {args.template_id}: {result.reason}")
        return 0
    if result.passed:
        print(
            f"PASS {args.template_id}: {result.passed_count} test(s) passed "
            f"(rendered + built in {result.tmp_dir})")
        return 0
    print(
        f"FAIL {args.template_id}: twister rc={result.returncode}, "
        f"{result.passed_count} test(s) passed", file=sys.stderr)
    if result.stdout:
        print(result.stdout, file=sys.stderr)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)

    render_p = sub.add_parser(
        "render", help="Materialise a template into dest_dir (or preview with --dry-run).")
    render_p.add_argument("template_id")
    render_p.add_argument("dest")
    render_p.add_argument("--dry-run", action="store_true",
                           help="List what would be written; touch nothing.")
    render_p.add_argument("--force", action="store_true",
                           help="Allow overwriting a non-empty dest_dir.")
    render_p.add_argument("--param", action="append", type=_parse_param,
                           metavar="name=value",
                           help="Override a declared template parameter "
                                "(repeatable).")
    render_p.add_argument("--catalog", type=Path, default=None,
                           help="Catalog JSON to read (default: "
                                "metadata/templates/catalog-v1.json).")
    render_p.set_defaults(func=_cli_render)

    validate_p = sub.add_parser(
        "validate",
        help="Render into a temp dir and run its native_sim test via twister.")
    validate_p.add_argument("template_id")
    validate_p.add_argument("--catalog", type=Path, default=None)
    validate_p.add_argument("--zephyr-base", default=None,
                             help="Default: $ZEPHYR_BASE.")
    validate_p.add_argument("--keep-tmp", action="store_true",
                             help="Don't delete the temp render dir "
                                  "(debugging).")
    validate_p.set_defaults(func=_cli_validate)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
