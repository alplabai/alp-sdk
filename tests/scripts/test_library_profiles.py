# SPDX-License-Identifier: Apache-2.0
"""
Regression test enforcing per-library HW-backend profile coverage
across the curated ``cores.<id>.libraries:`` enum in
``metadata/schemas/board.schema.json``.

Every library named in the schema enum MUST have a matching
``metadata/library-profiles/<libname>/hw-backends.yaml`` profile
shipped alongside it.  Without this guard, a maintainer can extend
the enum without dropping the per-library accelerator-binding table
the loader (``scripts/alp_project.py``) reads against each SoM's
``capabilities:`` block.

What the test enforces:

1. Coverage -- the set of library names appearing in
   ``metadata/library-profiles/`` exactly matches the schema enum.
   Profile directory names equal their board.yaml library token
   1:1 (``_LIB_TO_DIR`` is the empty override map kept for any
   future, deliberate divergence).

2. Shape -- each profile YAML has the required top-level fields
   (``schema_version``, ``library``, ``class``, ``accelerators``,
   ``sw_fallback``, ``verification``) and at least one of the three
   binding axes (``accelerators[]`` entries, ``sw_fallback``, or
   ``verification``).

3. ``library:`` field matches the directory name (post-normalisation).

4. Every Kconfig string emitted by a profile is a real-looking
   ``CONFIG_<NAME>=<value>`` token (or an explicit comment line --
   ``# foo: ...`` -- for profiles that have no Kconfig knob to
   surface, such as header-only libraries).

Out of scope (intentionally):

- We do NOT verify that the emitted ``CONFIG_*`` symbols exist in
  Zephyr's Kconfig tree.  That depends on the upstream Zephyr
  version pinned in west.yml and is checked at build time, not in
  the SDK's metadata regression suite.

- We do NOT verify the loader emits each accelerator line for a
  given SoM.  That's covered by ``test_project_backends.py`` (which
  feeds real SoM ``capabilities:`` blocks through
  ``_emit_library_hw_backends`` and asserts the cross-product).

Run locally::

    python -m pytest tests/scripts/test_library_profiles.py -v
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

import pytest
import yaml


REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "board.schema.json"
PROFILE_DIR = REPO / "metadata" / "library-profiles"


# Profile directory names match the board.yaml library token 1:1
# (e.g. `cmsis_dsp/`, `tflite_micro/`, `madgwick_ahrs/`).  This
# invariant is what the loader's profile-path lookup relies on
# (scripts/alp_project.py `_emit_library_hw_backends`).  Declare an
# explicit (currently empty) override map so any future token!=dir
# divergence triggers a deliberate decision rather than silent drift.
_LIB_TO_DIR: dict[str, str] = {}


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _schema_enum() -> list[str]:
    """Pull the ``cores.<id>.libraries:`` enum from the board schema.

    Walks the JSON tree until it finds the property named ``libraries``
    whose ``items.enum`` is a list of strings.  Centralised here so
    the test stays robust against minor reshuffles inside the schema.
    """
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

    def walk(node: Any) -> list[str] | None:
        if isinstance(node, dict):
            if (
                "libraries" in node
                and isinstance(node["libraries"], dict)
                and isinstance(node["libraries"].get("items"), dict)
                and isinstance(node["libraries"]["items"].get("enum"), list)
            ):
                enum = node["libraries"]["items"]["enum"]
                if all(isinstance(x, str) for x in enum):
                    return list(enum)
            for v in node.values():
                hit = walk(v)
                if hit is not None:
                    return hit
        elif isinstance(node, list):
            for x in node:
                hit = walk(x)
                if hit is not None:
                    return hit
        return None

    enum = walk(schema)
    assert enum is not None, (
        f"could not locate cores.<id>.libraries enum in {SCHEMA_PATH}"
    )
    return enum


def _profile_dirs() -> list[str]:
    """Names of all sub-directories under ``metadata/library-profiles/``
    that ship a ``hw-backends.yaml`` file.  Skips the top-level README."""
    out: list[str] = []
    for entry in sorted(PROFILE_DIR.iterdir()):
        if not entry.is_dir():
            continue
        if (entry / "hw-backends.yaml").is_file():
            out.append(entry.name)
    return out


def _lib_to_dir(lib: str) -> str:
    return _LIB_TO_DIR.get(lib, lib)


def _dir_to_lib(d: str) -> str:
    inv = {v: k for k, v in _LIB_TO_DIR.items()}
    return inv.get(d, d)


def _load_profile(libdir: str) -> dict[str, Any]:
    path = PROFILE_DIR / libdir / "hw-backends.yaml"
    with path.open(encoding="utf-8") as fh:
        doc = yaml.safe_load(fh)
    assert isinstance(doc, dict), (
        f"{path} did not parse to a mapping (got {type(doc).__name__})"
    )
    return doc


# Real-looking Kconfig line: CONFIG_<NAME>=<value> where <NAME> is
# upper alnum + underscore and <value> is y / n / m / quoted string /
# integer / hex.  Pre-compiled once and reused per call site.
_KCONFIG_RE = re.compile(
    r"^CONFIG_[A-Z][A-Z0-9_]*=(?:y|n|m|\"[^\"]*\"|0x[0-9A-Fa-f]+|-?\d+)$"
)

_STATUS_VALUES = {"implemented", "planned", "stub"}


def _is_real_kconfig(line: str) -> bool:
    """A bare ``CONFIG_FOO=y`` token.  Whitespace is stripped before
    matching so YAML formatters that pad scalars don't trip the regex.
    """
    return bool(_KCONFIG_RE.match(line.strip()))


def _is_comment_only_kconfig(line: str) -> bool:
    """Some profiles (header-only libraries) emit a ``"# foo: ..."``
    sentinel instead of a real Kconfig knob.  Accept those explicitly
    so the shape check doesn't reject the legitimate case.
    """
    return line.strip().startswith("#")


# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------


def test_every_enum_library_has_a_profile() -> None:
    """Every library named in the schema enum ships a matching
    ``hw-backends.yaml``.  Without this, the loader's
    ``_emit_library_hw_backends`` would skip the library silently."""
    enum = _schema_enum()
    dirs = set(_profile_dirs())

    missing: list[str] = []
    for lib in enum:
        if _lib_to_dir(lib) not in dirs:
            missing.append(lib)

    assert not missing, (
        f"{len(missing)} libraries in the schema enum lack a "
        f"hw-backends.yaml profile: {sorted(missing)}.  Drop one at "
        f"metadata/library-profiles/<lib>/hw-backends.yaml -- see "
        f"the existing mbedtls/ or lvgl/ profile for the shape."
    )


def test_no_orphan_profile_dirs() -> None:
    """The reverse direction: every shipped profile directory maps
    back to a library named in the schema enum.  Without this,
    ``metadata/library-profiles/`` could accumulate dead directories
    (e.g. for a library the enum dropped during a schema flatten)."""
    enum = set(_schema_enum())
    enum_dirs = {_lib_to_dir(lib) for lib in enum}

    orphans: list[str] = []
    for d in _profile_dirs():
        if d not in enum_dirs:
            orphans.append(d)

    assert not orphans, (
        f"{len(orphans)} profile directories have no matching enum "
        f"entry: {sorted(orphans)}.  Either add the library to the "
        f"cores.<id>.libraries enum in board.schema.json, or drop "
        f"the directory."
    )


@pytest.mark.parametrize("libdir", _profile_dirs(), ids=lambda d: d)
def test_profile_shape(libdir: str) -> None:
    """Each profile carries the canonical top-level fields and the
    ``library:`` slug matches the directory's normalised name.  The
    template is documented in ``metadata/library-profiles/README.md``
    and mirrored across the existing 25 profiles."""
    doc = _load_profile(libdir)

    # Required scalars + structural fields.
    for field in (
        "schema_version",
        "library",
        "class",
        "accelerators",
        "sw_fallback",
        "verification",
    ):
        assert field in doc, (
            f"{libdir}/hw-backends.yaml is missing required field "
            f"`{field}:` -- compare against the mbedtls/ profile."
        )

    # schema_version is currently locked at 1; if it bumps,
    # update this test in the same change.
    assert doc["schema_version"] == 1, (
        f"{libdir}/hw-backends.yaml has unexpected schema_version="
        f"{doc['schema_version']!r}; only v1 is supported today."
    )

    # `library:` must equal the canonical (underscore) form.
    canonical = _dir_to_lib(libdir)
    assert doc["library"] == canonical, (
        f"{libdir}/hw-backends.yaml declares library: {doc['library']!r} "
        f"but the directory normalises to {canonical!r}."
    )

    # `accelerators:` must be a list (possibly empty, in which case
    # the profile relies purely on `sw_fallback:`).  Each entry must
    # carry a `priority:` list.
    accels = doc["accelerators"]
    assert isinstance(accels, list), (
        f"{libdir}/hw-backends.yaml: `accelerators:` must be a list, "
        f"got {type(accels).__name__}"
    )
    for idx, entry in enumerate(accels):
        if entry is None:
            # An empty `accelerators: []` parses to a list with no
            # items; an explicit `- []` block parses to a None.  Skip
            # the latter cleanly.
            continue
        assert isinstance(entry, dict), (
            f"{libdir}/hw-backends.yaml: accelerators[{idx}] must "
            f"be a mapping (class: + priority:), got "
            f"{type(entry).__name__}"
        )
        assert "class" in entry, (
            f"{libdir}/hw-backends.yaml: accelerators[{idx}] missing "
            f"`class:` field"
        )
        assert "priority" in entry, (
            f"{libdir}/hw-backends.yaml: accelerators[{idx}] missing "
            f"`priority:` field"
        )

    # `sw_fallback:` must declare `required: true` (every library
    # needs a pure-SW floor) and carry a `kconfig:` line.
    fallback = doc["sw_fallback"]
    assert isinstance(fallback, dict), (
        f"{libdir}/hw-backends.yaml: `sw_fallback:` must be a mapping"
    )
    assert fallback.get("required") is True, (
        f"{libdir}/hw-backends.yaml: `sw_fallback.required:` must be "
        f"true -- every library needs a pure-SW floor"
    )
    assert "kconfig" in fallback, (
        f"{libdir}/hw-backends.yaml: `sw_fallback.kconfig:` is required"
    )


@pytest.mark.parametrize("libdir", _profile_dirs(), ids=lambda d: d)
def test_profile_has_at_least_one_binding_axis(libdir: str) -> None:
    """A profile must surface at least one of: a populated
    ``accelerators[]`` entry, a ``sw_fallback:`` Kconfig knob, or a
    ``verification:`` block.  An empty profile is meaningless to the
    loader and a sign the library was added to the enum without a
    real coverage decision."""
    doc = _load_profile(libdir)
    accels = doc.get("accelerators") or []
    fallback = doc.get("sw_fallback") or {}
    verification = doc.get("verification") or {}

    has_accel = any(
        isinstance(a, dict) and a.get("priority")
        for a in accels
    )
    has_fallback = bool(fallback.get("kconfig"))
    has_verification = bool(verification)

    assert has_accel or has_fallback or has_verification, (
        f"{libdir}/hw-backends.yaml has neither an `accelerators[]` "
        f"entry, a `sw_fallback.kconfig:` knob, nor a `verification:` "
        f"block.  At least one is required."
    )


@pytest.mark.parametrize("libdir", _profile_dirs(), ids=lambda d: d)
def test_kconfig_lines_well_formed(libdir: str) -> None:
    """Every ``kconfig:`` string in the profile is a real-looking
    ``CONFIG_<NAME>=<value>`` token, OR a comment-only sentinel for
    header-only libraries.  We do NOT validate that the symbol exists
    in Zephyr's Kconfig tree (that's a build-time concern that
    depends on the pinned Zephyr version)."""
    doc = _load_profile(libdir)

    # Collect every kconfig: line across the profile.
    lines: list[tuple[str, str]] = []  # (location, line)

    for idx, entry in enumerate(doc.get("accelerators") or []):
        if not isinstance(entry, dict):
            continue
        for jdx, prio in enumerate(entry.get("priority") or []):
            if not isinstance(prio, dict):
                continue
            kc = prio.get("kconfig")
            if kc is None:
                continue
            lines.append((f"accelerators[{idx}].priority[{jdx}]", str(kc)))

    fallback = doc.get("sw_fallback") or {}
    if "kconfig" in fallback:
        lines.append(("sw_fallback", str(fallback["kconfig"])))

    bad: list[str] = []
    for location, line in lines:
        if _is_comment_only_kconfig(line):
            continue
        if _is_real_kconfig(line):
            continue
        bad.append(f"  {location}: {line!r}")

    assert not bad, (
        f"{libdir}/hw-backends.yaml has malformed kconfig: lines:\n"
        + "\n".join(bad)
        + "\nExpected CONFIG_<NAME>=<value> (where value is y / n / m / "
        '"string" / integer / 0xhex) or a "# ..." comment for '
        "header-only libraries."
    )


@pytest.mark.parametrize("libdir", _profile_dirs(), ids=lambda d: d)
def test_accelerator_status_values_well_formed(libdir: str) -> None:
    """Optional accelerator ``status:`` values are part of the
    loader contract.  Missing means ``implemented``; planned/stub
    entries document intended bindings but must not emit active
    Kconfig claims."""
    doc = _load_profile(libdir)

    bad: list[str] = []
    for idx, entry in enumerate(doc.get("accelerators") or []):
        if not isinstance(entry, dict):
            continue
        for jdx, prio in enumerate(entry.get("priority") or []):
            if not isinstance(prio, dict):
                continue
            status = str(prio.get("status", "implemented")).strip().lower()
            if status not in _STATUS_VALUES:
                bad.append(
                    f"accelerators[{idx}].priority[{jdx}] status={status!r}"
                )

    assert not bad, (
        f"{libdir}/hw-backends.yaml has unsupported accelerator status "
        f"value(s): {bad}. Expected one of {sorted(_STATUS_VALUES)}."
    )
