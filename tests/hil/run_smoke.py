#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
HiL smoke-test runner for the ALP SDK.

Drives one or more YAML smoke specs against attached hardware:

  1. Build the named example with `west build -b <board>` (Zephyr
     module loading wires in alp-sdk + the per-SoM board target).
  2. Flash the resulting image to the EVK.
  3. Capture serial output for `serial.duration_s` seconds.
  4. Assert every `expect_contains` string appears, every
     `expect_absent` string does not.

Modes:

  --validate <path>     Parse + schema-check every spec under <path>;
                        no hardware access.
  --dry-run <path>      Print the build / flash / capture commands
                        each spec would run; no hardware access.
  (default) <path>      Full run: build, flash, capture, assert.

Exit codes:
  0  every spec passed
  1  one or more specs failed (assertion miss, build error, …)
  2  invocation error (missing path, malformed spec, …)

See tests/hil/README.md for the spec format and the runner-host
contract; docs/ci/HW-IN-LOOP.md for runner-side setup.
"""

from __future__ import annotations

import argparse
import dataclasses as dc
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover -- environmental
    sys.exit("run_smoke: PyYAML is required.  Install via `pip install pyyaml`.")


REPO = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------
# Spec model
# ---------------------------------------------------------------------


@dc.dataclass(frozen=True)
class SerialSpec:
    duration_s: int
    baud: int
    expect_contains: tuple[str, ...]
    expect_absent: tuple[str, ...]


@dc.dataclass(frozen=True)
class SmokeSpec:
    """A single resolved smoke spec (after merging with _runner.yaml)."""
    name: str
    description: str
    example: Path                # repo-relative
    board: str
    serial_port: str
    flash_method: str
    serial: SerialSpec
    source_path: Path            # for error messages


@dc.dataclass(frozen=True)
class SmokeResult:
    spec: SmokeSpec
    ok: bool
    failures: tuple[str, ...]    # empty when ok=True


# ---------------------------------------------------------------------
# Spec parsing
# ---------------------------------------------------------------------


_REQUIRED_TOP = ("schema_version", "name", "example", "serial")
_SERIAL_REQUIRED = ("expect_contains",)


class SpecError(ValueError):
    """Raised for any spec-parsing failure.  Carries the file path."""


def _load_yaml(path: Path) -> dict[str, Any]:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        raise SpecError(f"{path}: invalid YAML ({e})") from e
    if not isinstance(data, dict):
        raise SpecError(f"{path}: top-level value must be a mapping")
    return data


def _load_runner_defaults(runner_or_dir: Path) -> dict[str, Any]:
    """Read a `_runner.yaml` file.  If `runner_or_dir` is a directory,
    look for `_runner.yaml` inside it; if it's a file, read it
    directly.  Returns {} if no runner.yaml is found."""
    if runner_or_dir.is_dir():
        runner_path = runner_or_dir / "_runner.yaml"
    else:
        runner_path = runner_or_dir
    if not runner_path.is_file():
        return {}
    return _load_yaml(runner_path)


def _merge_serial(
    runner_default: dict[str, Any], spec_serial: dict[str, Any],
) -> SerialSpec:
    rd = runner_default.get("defaults", {}).get("serial", {}) or {}
    duration = spec_serial.get("duration_s", rd.get("duration_s", 30))
    baud = spec_serial.get("baud", rd.get("baud", 115200))
    contains = tuple(spec_serial.get("expect_contains") or ())
    absent = tuple(spec_serial.get("expect_absent") or ())
    if not contains:
        raise SpecError("serial.expect_contains must list at least one string")
    return SerialSpec(
        duration_s=int(duration),
        baud=int(baud),
        expect_contains=contains,
        expect_absent=absent,
    )


def parse_spec(spec_path: Path, runner_path: Path | None = None) -> SmokeSpec:
    """Parse a single spec file + merge with a `_runner.yaml`.

    When `runner_path` is None (the default), looks for `_runner.yaml`
    next to `spec_path`.  When supplied (used by the
    `_common/`-aware discovery flow), reads from that path -- this is
    how shared specs in `_common/` inherit the per-board runner's
    board target + serial port.

    `runner_path` may be a file (`<board>/_runner.yaml`) or a
    directory (the function appends `_runner.yaml`)."""
    data = _load_yaml(spec_path)
    for k in _REQUIRED_TOP:
        if k not in data:
            raise SpecError(f"{spec_path}: missing required key '{k}'")
    if int(data["schema_version"]) != 1:
        raise SpecError(
            f"{spec_path}: unsupported schema_version {data['schema_version']!r} "
            "(this runner understands schema_version: 1)"
        )

    runner_target = runner_path if runner_path is not None else spec_path.parent
    runner = _load_runner_defaults(runner_target)
    board = data.get("board") or runner.get("board")
    if not board:
        raise SpecError(
            f"{spec_path}: no `board:` declared and no _runner.yaml default"
        )
    serial_port = data.get("serial_port") or runner.get("serial_port", "/dev/ttyACM0")
    flash_method = data.get("flash_method") or runner.get("flash_method", "westflash")

    example = Path(data["example"])
    if not example.is_absolute():
        example_abs = REPO / example
    else:
        example_abs = example
    if not example_abs.is_dir():
        raise SpecError(
            f"{spec_path}: example path does not exist: {example_abs}"
        )

    spec_serial = data["serial"]
    if not isinstance(spec_serial, dict):
        raise SpecError(f"{spec_path}: `serial:` must be a mapping")
    serial = _merge_serial(runner, spec_serial)

    return SmokeSpec(
        name=data["name"],
        description=data.get("description", "").strip(),
        example=example_abs,
        board=str(board),
        serial_port=str(serial_port),
        flash_method=str(flash_method),
        serial=serial,
        source_path=spec_path,
    )


def discover_specs(target: Path) -> list[Path]:
    """If target is a file, return [target].  If a directory, return
    every *.yaml in it sorted alphabetically, skipping `_runner.yaml`
    and any file starting with '_' (treated as private)."""
    if target.is_file():
        return [target]
    if not target.is_dir():
        raise SpecError(f"not a file or directory: {target}")
    return sorted(
        p for p in target.glob("*.yaml")
        if not p.name.startswith("_")
    )


# Default location of the shared spec library, relative to the runner.
_COMMON_DIR = Path(__file__).resolve().parent / "_common"


def discover_specs_for_board(
    board_dir: Path,
    *,
    include_common: bool = True,
    common_dir: Path = _COMMON_DIR,
) -> list[tuple[Path, Path]]:
    """Pair every spec for `board_dir` with the runner.yaml it
    inherits.

    Returns a list of ``(spec_path, runner_yaml_path)`` tuples,
    sorted by spec name.  The runner_yaml_path is always
    `board_dir/_runner.yaml` so shared `_common/` specs inherit the
    board's target + serial port.

    Override semantics: a board-local spec named identically to a
    shared `_common/` spec wins.  This is how a board specialises a
    portable smoke (e.g. with a tighter `serial.duration_s` or extra
    `expect_contains` strings).

    When `include_common=False` (the --no-common CLI flag), only
    `board_dir`'s own specs are returned -- useful when a board's
    spec set deliberately excludes the portable library."""
    if not board_dir.is_dir():
        raise SpecError(f"not a directory: {board_dir}")

    board_runner = board_dir / "_runner.yaml"
    own_specs = [
        p for p in board_dir.glob("*.yaml")
        if not p.name.startswith("_")
    ]

    pairs: list[tuple[Path, Path]] = [(p, board_runner) for p in own_specs]

    if include_common and common_dir.is_dir():
        own_names = {p.name for p in own_specs}
        for p in common_dir.glob("*.yaml"):
            if p.name.startswith("_"):
                continue
            if p.name in own_names:
                continue              # board override wins
            pairs.append((p, board_runner))

    pairs.sort(key=lambda pair: pair[0].name)
    return pairs


def is_board_dir(target: Path) -> bool:
    """Return True when `target` looks like a board directory --
    i.e. it's a directory under tests/hil/ that carries a
    `_runner.yaml` AND isn't `_common/` itself."""
    if not target.is_dir():
        return False
    if target.resolve() == _COMMON_DIR.resolve():
        return False
    return (target / "_runner.yaml").is_file()


# ---------------------------------------------------------------------
# Command builders (pure -- emit the argv lists; the run loop executes)
# ---------------------------------------------------------------------


def build_command(spec: SmokeSpec) -> list[str]:
    """`west build -p always -b <board> <example>`.  The runner host's
    workspace is expected to have alp-sdk on the module path (CI sets
    EXTRA_ZEPHYR_MODULES; humans use a west-init workspace)."""
    return [
        "west", "build", "-p", "always",
        "-b", spec.board,
        str(spec.example),
    ]


def flash_command(spec: SmokeSpec) -> list[str]:
    """Map flash_method to a shell command.  `westflash` is the
    default; `pyocd-flash` is the explicit J-Link / DAPLink path."""
    if spec.flash_method == "westflash":
        return ["west", "flash"]
    if spec.flash_method == "pyocd-flash":
        return ["pyocd", "flash", "--target", spec.board,
                "build/zephyr/zephyr.elf"]
    raise SpecError(
        f"{spec.source_path}: unknown flash_method '{spec.flash_method}' "
        "(supported: westflash, pyocd-flash)"
    )


def capture_command(spec: SmokeSpec) -> list[str]:
    """Invoke the runner-host helper that captures serial output.
    The script (documented in docs/ci/HW-IN-LOOP.md) ships at
    /opt/alp-hil/capture-serial.sh; humans can substitute via
    --capture-cmd."""
    return [
        "/opt/alp-hil/capture-serial.sh",
        "--port", spec.serial_port,
        "--duration", str(spec.serial.duration_s),
        "--output", "/tmp/hil-output.log",
    ]


# ---------------------------------------------------------------------
# Assertion engine
# ---------------------------------------------------------------------


def assert_serial(spec: SmokeSpec, captured: str) -> list[str]:
    """Return a list of human-readable failure messages.  Empty list
    means every assertion passed."""
    failures: list[str] = []
    haystack = captured.lower()
    for needle in spec.serial.expect_contains:
        if needle.lower() not in haystack:
            failures.append(f"missing expected: {needle!r}")
    for needle in spec.serial.expect_absent:
        if needle.lower() in haystack:
            failures.append(f"saw forbidden: {needle!r}")
    return failures


# ---------------------------------------------------------------------
# Drivers
# ---------------------------------------------------------------------


def _run(cmd: list[str]) -> tuple[int, str]:
    """Run a subprocess and capture combined stdout+stderr.  Returns
    (returncode, output).  Doesn't raise on non-zero exit -- the
    caller decides whether to fail the spec."""
    proc = subprocess.run(
        cmd, capture_output=True, text=True, check=False,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def run_spec(spec: SmokeSpec, *, dry_run: bool = False) -> SmokeResult:
    """Build, flash, capture, assert.  When dry_run=True, print
    each command and skip execution."""
    if dry_run:
        for label, cmd in (("build", build_command(spec)),
                           ("flash", flash_command(spec)),
                           ("capture", capture_command(spec))):
            print(f"  [{label}] " + " ".join(cmd))
        return SmokeResult(spec=spec, ok=True, failures=())

    # 1. Build.
    rc, out = _run(build_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"build failed (rc={rc}): {out.strip()[:400]}",))

    # 2. Flash.
    rc, out = _run(flash_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"flash failed (rc={rc}): {out.strip()[:400]}",))

    # 3. Capture serial.
    rc, out = _run(capture_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"capture failed (rc={rc}): {out.strip()[:400]}",))
    captured = Path("/tmp/hil-output.log").read_text(encoding="utf-8", errors="replace") \
        if Path("/tmp/hil-output.log").exists() else out

    # 4. Assert.
    failures = assert_serial(spec, captured)
    return SmokeResult(spec, not failures, tuple(failures))


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------


def _print_summary(results: list[SmokeResult]) -> int:
    passed = sum(1 for r in results if r.ok)
    failed = [r for r in results if not r.ok]
    print()
    print(f"===== HiL smoke summary ({passed}/{len(results)} passed) =====")
    for r in results:
        marker = "PASS" if r.ok else "FAIL"
        print(f"  [{marker}] {r.spec.name}  ({r.spec.source_path.name})")
        for f in r.failures:
            print(f"     - {f}")
    return 0 if not failed else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="HiL smoke-test runner for the ALP SDK.",
    )
    parser.add_argument(
        "target", type=Path,
        help="Spec file (*.yaml), a board directory with a "
             "_runner.yaml (then both _common/ and the board's own "
             "specs run), or a plain directory of specs.",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--validate", action="store_true",
                      help="Parse + schema-check specs; no hardware.")
    mode.add_argument("--dry-run", action="store_true",
                      help="Print build / flash / capture commands; no hardware.")
    parser.add_argument(
        "--no-common", action="store_true",
        help="When target is a board directory, suppress the "
             "_common/ portable spec set; run only the board's "
             "own specs.  Useful for board-specific smoke sweeps.",
    )
    args = parser.parse_args()

    if not args.target.exists():
        print(f"run_smoke: target not found: {args.target}", file=sys.stderr)
        return 2

    # Resolution: board-dir mode (with optional _common/ inclusion)
    # vs plain target mode.
    try:
        if is_board_dir(args.target):
            pairs = discover_specs_for_board(
                args.target, include_common=not args.no_common,
            )
        else:
            spec_paths = discover_specs(args.target)
            pairs = [(p, None) for p in spec_paths]
    except SpecError as e:
        print(f"run_smoke: {e}", file=sys.stderr)
        return 2

    if not pairs:
        print(f"run_smoke: no spec files under {args.target}", file=sys.stderr)
        return 2

    # Parse every spec first; surfaces malformed specs cleanly.
    specs: list[SmokeSpec] = []
    for spec_path, runner_path in pairs:
        try:
            specs.append(parse_spec(spec_path, runner_path=runner_path))
        except SpecError as e:
            print(f"run_smoke: {e}", file=sys.stderr)
            return 2

    if args.validate:
        for s in specs:
            print(f"OK  {s.source_path}  -> {s.name} on {s.board}")
        print(f"\nrun_smoke: {len(specs)} spec(s) validated")
        return 0

    if args.dry_run:
        for s in specs:
            print(f"\n--- {s.source_path} ---")
            run_spec(s, dry_run=True)
        return 0

    # Real run -- pre-flight check that west is available.
    if not shutil.which("west"):
        print("run_smoke: `west` not on PATH -- HiL runs need the "
              "Zephyr workspace + west.", file=sys.stderr)
        return 2

    results = [run_spec(s) for s in specs]
    return _print_summary(results)


if __name__ == "__main__":
    sys.exit(main())
