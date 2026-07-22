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
    --sku <SKU>` (issue #864): renders entirely in memory (no dest_dir,
    no disk write) and returns `[(path, contents), ...]` in the same
    sorted order render() writes, sharing the same per-file read/
    substitute loop so the two can never disagree on a byte. `sku` must
    be one of the template's declared `supported.som_skus`
    (SkuNotSupportedError otherwise, naming the supported set); the
    rendered `board.yaml`'s `som.sku:` and top-level `preset:` lines are
    substituted for `sku`'s own default board (metadata/e1m_modules/
    <sku>.yaml `default_board:`), a no-op (byte-identical passthrough)
    when `sku` already matches the example's own default. This is the
    SDK-owned single source of the scaffold's build-integration
    conventions -- the surface tan-cli vendors at release instead of
    hand-porting a per-SKU Rust generator.

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

def _default_preset_for_sku(sku: str, metadata_root: Path) -> str:
    """The board preset a fresh project targeting `sku` ships with by
    default -- metadata/e1m_modules/<sku>.yaml's `default_board:`,
    lower-cased to match the `preset:` value every example board.yaml
    already uses (e.g. `E1M-EVK` -> `e1m-evk`, `E1M-X-EVK` ->
    `e1m-x-evk`). This is the SAME field board.yaml's own comments point
    customers at by hand ("copy this directory, change som.sku ...,
    edit the preset:") -- render_to_envelope just does that edit for
    them."""
    som_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not som_path.is_file():
        raise TemplateError(
            f"no metadata/e1m_modules/{sku}.yaml for sku {sku!r}")
    doc = yaml.safe_load(som_path.read_text(encoding="utf-8")) or {}
    board = doc.get("default_board")
    if not board:
        raise TemplateError(
            f"metadata/e1m_modules/{sku}.yaml has no default_board")
    return board.lower()


# Matches board.yaml's `som:\n  sku: E1M-...` line (only the token right
# after `sku:`, so any trailing inline comment survives untouched) and
# the top-level `preset: <name>` line. count=1 each: board.yaml declares
# exactly one of each.
_SOM_SKU_RE = re.compile(r"(?m)^(\s*sku:\s*)E1M-[A-Z0-9]+")
_PRESET_RE = re.compile(r"(?m)^(preset:\s*)\S+")


def _substitute_board_yaml_sku(text: str, sku: str, preset: str) -> str:
    text, n_sku = _SOM_SKU_RE.subn(rf"\g<1>{sku}", text, count=1)
    if n_sku == 0:
        raise TemplateError("board.yaml has no `som.sku:` line to substitute")
    text, n_preset = _PRESET_RE.subn(rf"\g<1>{preset}", text, count=1)
    if n_preset == 0:
        raise TemplateError("board.yaml has no top-level `preset:` line to substitute")
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
    `sharedArtefacts` already use.

    `sku` MUST be one of the record's declared `supported.som_skus` --
    SkuNotSupportedError (naming the supported set) otherwise, never a
    silent best-effort render. The rendered `board.yaml`'s `som.sku:`
    and top-level `preset:` are substituted for `sku`'s own default
    board (metadata/e1m_modules/<sku>.yaml `default_board:`); when
    `sku` already matches the example's own default this substitution
    is a no-op, so the output is byte-identical to the example (a
    passthrough). Every other user_owned file is an unmodified copy --
    reuses `_rendered_bytes()`, so it can never disagree with render()
    on a byte.
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
    preset = _default_preset_for_sku(sku, metadata_root or METADATA_ROOT)

    out: list[tuple[str, str]] = []
    for rel, data in _rendered_bytes(template_id, record, render_plan.files, resolved, base):
        if rel == "board.yaml":
            data = _substitute_board_yaml_sku(
                data.decode("utf-8"), sku, preset).encode("utf-8")
        out.append((rel, data.decode("utf-8")))
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
