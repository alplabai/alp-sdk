#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
HiL smoke-test runner for the Alp SDK.

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
  (default) <path>      Full run: build, flash, capture, assert.  Needs
                        a resolved serial port -- pass --serial-port or
                        set ALP_HIL_SERIAL_PORT (no hardcoded default;
                        see docs/ci/HW-IN-LOOP.md).

flash_method: ssh-run is for Linux (A55) examples: no west build and no
serial console.  The example's binary is built beforehand (Yocto or the
SDK toolchain) and found as <--artifact-dir>/<example dir name>; the
runner copies it to the target with scp, runs it over ssh, and applies
the spec's `serial:` expectations to its output.  The target comes from
--ssh-host or ALP_HIL_SSH_HOST (no default, same rule as the serial port).
A spec may also carry `ssh_args: [...]` (argv passed to the remote
binary) and `ssh_files: [{local: <path relative to --artifact-dir>,
remote: /tmp/...}]` (extra inputs -- a model bundle, input frames -- scp'd
alongside the binary before it runs); both are optional and default to
none.  Instead of an example, an ssh-run spec may carry `ssh_command:` -- a
read-only shell one-liner run on the target over plain ssh (no binary, no
scp) -- for checks of the running image (dmesg, /dev, systemd).

Exit codes:
  0  every spec passed
  1  one or more specs failed (assertion miss, build error, …)
  2  invocation error (missing path, malformed spec, …)

CI does not drive this runner -- it's invoked by hand on the bench,
under a held labgrid reservation.  See tests/hil/README.md for the
spec format; docs/ci/HW-IN-LOOP.md for the bench-run contract
(hardware, serial-port resolution, the capture helper).
"""

from __future__ import annotations

import argparse
import dataclasses as dc
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
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
    example: Path | None         # repo-relative; None for ssh_command specs
    board: str
    serial_port: str
    flash_method: str
    serial: SerialSpec
    source_path: Path            # for error messages
    ssh_host: str = ""           # ssh-run: resolved from --ssh-host / env
    artifact_dir: str = ""       # ssh-run: where prebuilt binaries live
    ssh_args: tuple[str, ...] = ()          # ssh-run: argv passed to the remote binary
    ssh_command: str = ""        # ssh-run: read-only shell one-liner run on the
    # target INSTEAD of a prebuilt example binary (no example, no scp)
    ssh_files: tuple[tuple[str, str], ...] = ()  # ssh-run: (local rel to
    # --artifact-dir, remote path) pairs scp'd before the binary runs --
    # e.g. a model bundle tar / input frames an example takes as argv,
    # not just the binary itself (#1160: v2n-drpai-inference needs both).


@dc.dataclass(frozen=True)
class SmokeResult:
    spec: SmokeSpec
    ok: bool
    failures: tuple[str, ...]    # empty when ok=True


# ---------------------------------------------------------------------
# Spec parsing
# ---------------------------------------------------------------------


_REQUIRED_TOP = ("schema_version", "name", "serial")
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
    # No hardcoded default: the bench fronts UART devices with labgrid,
    # which allocates a ser2net port on reservation ACQUIRE -- it differs
    # per session, and a raw /dev/ttyUSB*/ttyACM* path isn't durable
    # either (worse, /dev/ttyACM0 on this bench is the DPS-150 power
    # supply, not a console -- see docs/ci/HW-IN-LOOP.md).  An empty
    # string here just means "not resolved yet"; --validate/--dry-run
    # don't need it, and main() resolves it from --serial-port /
    # ALP_HIL_SERIAL_PORT before a real run, erroring clearly if it's
    # still unresolved at that point (see run_spec's capture guard).
    serial_port = data.get("serial_port") or runner.get("serial_port") or ""
    flash_method = data.get("flash_method") or runner.get("flash_method", "westflash")

    ssh_command = str(data.get("ssh_command") or "").strip()
    if ssh_command:
        if flash_method != SSH_RUN:
            raise SpecError(f"{spec_path}: ssh_command needs flash_method: ssh-run")
        if "example" in data or data.get("ssh_files") or data.get("ssh_args"):
            raise SpecError(
                f"{spec_path}: ssh_command excludes example / ssh_args / ssh_files"
            )
        example_abs = None
    else:
        if "example" not in data:
            raise SpecError(f"{spec_path}: missing required key 'example'")
        example = Path(data["example"])
        example_abs = example if example.is_absolute() else REPO / example
        if not example_abs.is_dir():
            raise SpecError(
                f"{spec_path}: example path does not exist: {example_abs}"
            )

    spec_serial = data["serial"]
    if not isinstance(spec_serial, dict):
        raise SpecError(f"{spec_path}: `serial:` must be a mapping")
    serial = _merge_serial(runner, spec_serial)

    ssh_args = tuple(str(a) for a in (data.get("ssh_args") or ()))

    ssh_files: list[tuple[str, str]] = []
    for entry in data.get("ssh_files") or ():
        if not isinstance(entry, dict) or "local" not in entry or "remote" not in entry:
            raise SpecError(
                f"{spec_path}: each ssh_files entry needs 'local' and 'remote' keys"
            )
        ssh_files.append((str(entry["local"]), str(entry["remote"])))

    return SmokeSpec(
        name=data["name"],
        description=data.get("description", "").strip(),
        example=example_abs,
        board=str(board),
        serial_port=str(serial_port),
        flash_method=str(flash_method),
        serial=serial,
        source_path=spec_path,
        ssh_args=ssh_args,
        ssh_files=tuple(ssh_files),
        ssh_command=ssh_command,
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


SSH_RUN = "ssh-run"


def _artifact_name(spec: SmokeSpec) -> str:
    assert spec.example is not None  # ssh_command specs never reach here
    return spec.example.name


def build_command(spec: SmokeSpec) -> list[str] | None:
    """`west build -p always -b <board> <example>`.  The runner host's
    workspace is expected to have alp-sdk on the module path (CI sets
    EXTRA_ZEPHYR_MODULES; humans use a west-init workspace).  None for
    ssh-run: that binary is prebuilt."""
    if spec.flash_method == SSH_RUN:
        return None
    assert spec.example is not None
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
    if spec.flash_method == SSH_RUN and spec.ssh_command:
        raise SpecError(f"{spec.source_path}: ssh_command specs have no copy step")
    if spec.flash_method == SSH_RUN:
        host = spec.ssh_host or "<unresolved: pass --ssh-host or set ALP_HIL_SSH_HOST>"
        local = Path(spec.artifact_dir or "<artifact-dir>") / _artifact_name(spec)
        return ["scp", "-q", str(local), f"{host}:/tmp/{_artifact_name(spec)}"]
    raise SpecError(
        f"{spec.source_path}: unknown flash_method '{spec.flash_method}' "
        "(supported: westflash, pyocd-flash, ssh-run)"
    )


def ssh_extra_file_commands(spec: SmokeSpec) -> list[list[str]]:
    """One `scp` command per `ssh_files:` entry -- copies extra inputs
    (a model bundle, input frames, ...) an ssh-run example takes as
    argv besides the binary itself.  Runs after the binary copy, same
    ordering the shell invocation in capture_command() assumes isn't
    load-bearing (the example only runs once every copy is done, in
    _run_ssh_spec)."""
    host = spec.ssh_host or "<unresolved: pass --ssh-host or set ALP_HIL_SSH_HOST>"
    base = Path(spec.artifact_dir or "<artifact-dir>")
    return [
        ["scp", "-q", str(base / local), f"{host}:{remote}"]
        for local, remote in spec.ssh_files
    ]


def _capture_output_path(spec: SmokeSpec) -> Path:
    """A fresh, per-process, per-spec capture path -- NEVER a single
    fixed log file.  A previous invocation's log (this process, an
    earlier one, or a different spec in the same run) can never share
    this path, so a capture that silently fails to write can never be
    asserted against stale content and read as a false PASS (the bug
    a single shared /tmp/hil-output.log had)."""
    return Path(tempfile.gettempdir()) / f"alp-hil-{os.getpid()}-{spec.name}.log"


def capture_command(spec: SmokeSpec) -> list[str]:
    """Invoke the serial-capture helper (documented in
    docs/ci/HW-IN-LOOP.md; /opt/alp-hil/capture-serial.sh by
    convention).  spec.serial_port must be resolved by the caller
    before a REAL run -- see run_spec's guard -- but this builder stays
    pure/side-effect-free so --dry-run can still print it when the
    port isn't resolved yet (--dry-run touches no hardware)."""
    if spec.flash_method == SSH_RUN and spec.ssh_command:
        host = spec.ssh_host or "<unresolved: pass --ssh-host or set ALP_HIL_SSH_HOST>"
        return ["ssh", host, spec.ssh_command]
    if spec.flash_method == SSH_RUN:
        host = spec.ssh_host or "<unresolved: pass --ssh-host or set ALP_HIL_SSH_HOST>"
        remote = f"/tmp/{_artifact_name(spec)}"
        # ssh_args (e.g. a model tar's remote path + frame paths, see
        # #1160) become argv to the remote binary -- shlex-quoted so a
        # spec author's path/flag survives the remote shell unmangled.
        argv = " ".join(shlex.quote(a) for a in spec.ssh_args)
        invocation = f"{remote} {argv}" if argv else remote
        # Bound the run to serial.duration_s like the serial path: some
        # examples (v2n-power-monitor) loop forever.  The target's busybox
        # has no `timeout`, so a background sleeper kills the example.
        # -tt gives it a pty, so its stdout is line-buffered: a killed
        # example would otherwise lose everything still in its buffer.
        secs = spec.serial.duration_s
        return ["ssh", "-tt", host,
                f"chmod +x {remote} && {{ {invocation} & p=$!; "
                f"(sleep {secs}; kill $p) >/dev/null 2>&1 & w=$!; "
                f"wait $p; kill $w 2>/dev/null; }}"]
    port = spec.serial_port or "<unresolved: pass --serial-port or set ALP_HIL_SERIAL_PORT>"
    return [
        "/opt/alp-hil/capture-serial.sh",
        "--port", port,
        "--duration", str(spec.serial.duration_s),
        "--output", str(_capture_output_path(spec)),
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
        cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"}, check=False,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def run_spec(spec: SmokeSpec, *, dry_run: bool = False) -> SmokeResult:
    """Build, flash, capture, assert.  When dry_run=True, print
    each command and skip execution."""
    if dry_run:
        for label, cmd in (("build", build_command(spec)),
                           ("flash", None if spec.ssh_command else flash_command(spec)),
                           *(("copy", c) for c in ssh_extra_file_commands(spec)),
                           ("capture", capture_command(spec))):
            if cmd is not None:
                print(f"  [{label}] " + " ".join(cmd))
        return SmokeResult(spec=spec, ok=True, failures=())

    if spec.flash_method == SSH_RUN:
        return _run_ssh_spec(spec)

    # 1. Build.
    rc, out = _run(build_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"build failed (rc={rc}): {out.strip()[:400]}",))

    # 2. Flash.
    rc, out = _run(flash_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"flash failed (rc={rc}): {out.strip()[:400]}",))

    # 3. Capture serial.  A real run (unlike --validate/--dry-run) must
    # have a resolved port -- there is no default to fall back to (see
    # docs/ci/HW-IN-LOOP.md: the bench allocates it per labgrid
    # reservation).
    if not spec.serial_port:
        return SmokeResult(spec, False, (
            "no serial port resolved -- pass --serial-port or set "
            "ALP_HIL_SERIAL_PORT (the bench's labgrid reservation "
            "allocates a ser2net port per session; there is no fixed "
            "default -- see docs/ci/HW-IN-LOOP.md).",
        ))
    capture_path = _capture_output_path(spec)
    capture_path.unlink(missing_ok=True)  # belt-and-suspenders: the path
    # is already unique per (pid, spec), so this can't remove another
    # run's data -- it only guards a leftover from a killed prior
    # invocation that reused this same pid+spec-name combination.
    rc, out = _run(capture_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"capture failed (rc={rc}): {out.strip()[:400]}",))
    captured = capture_path.read_text(encoding="utf-8", errors="replace") \
        if capture_path.exists() else out

    # 4. Assert.
    failures = assert_serial(spec, captured)
    return SmokeResult(spec, not failures, tuple(failures))


def _run_ssh_spec(spec: SmokeSpec) -> SmokeResult:
    """ssh-run: copy the prebuilt binary (+ any ssh_files: extra inputs),
    run it, assert on its output."""
    if not spec.ssh_host:
        return SmokeResult(spec, False, (
            "no ssh host resolved -- pass --ssh-host or set ALP_HIL_SSH_HOST",))
    if spec.ssh_command:
        _, out = _run(capture_command(spec))
        failures = assert_serial(spec, out)
        return SmokeResult(spec, not failures, tuple(failures))
    artifact = Path(spec.artifact_dir) / _artifact_name(spec)
    if not spec.artifact_dir or not artifact.is_file():
        return SmokeResult(spec, False, (
            f"prebuilt binary not found: {artifact} -- build the example for "
            "the target and pass --artifact-dir",))
    for local_rel, _remote in spec.ssh_files:
        local = Path(spec.artifact_dir) / local_rel
        if not local.is_file():
            return SmokeResult(spec, False, (
                f"ssh_files input not found: {local} -- see the spec's "
                "ssh_files: list and --artifact-dir",))
    rc, out = _run(flash_command(spec))
    if rc != 0:
        return SmokeResult(spec, False, (f"copy failed (rc={rc}): {out.strip()[:400]}",))
    for cmd in ssh_extra_file_commands(spec):
        rc, out = _run(cmd)
        if rc != 0:
            return SmokeResult(spec, False, (
                f"ssh_files copy failed (rc={rc}): {out.strip()[:400]}",))
    # The example's exit code is not asserted: examples report PASS/FAIL
    # on stdout, which the spec's expectations check.
    _, out = _run(capture_command(spec))
    failures = assert_serial(spec, out)
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
        description="HiL smoke-test runner for the Alp SDK.",
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
    parser.add_argument(
        "--serial-port",
        help="Serial device/port to capture from for THIS invocation "
             "(e.g. a ser2net host:port the bench's labgrid reservation "
             "allocated this session).  Overrides serial_port in every "
             "resolved spec.  Falls back to the ALP_HIL_SERIAL_PORT env "
             "var when omitted -- there is no hardcoded default; see "
             "docs/ci/HW-IN-LOOP.md.",
    )
    parser.add_argument(
        "--ssh-host",
        help="ssh-run specs: the target to run on (e.g. root@<board-ip>).  "
             "Falls back to ALP_HIL_SSH_HOST; no default.",
    )
    parser.add_argument(
        "--artifact-dir", type=Path,
        help="ssh-run specs: directory holding each example's prebuilt "
             "binary, named after the example directory.",
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

    # Resolve the serial port for this invocation -- CLI flag, then env
    # var, then whatever (if anything) the specs/_runner.yaml carried.
    # No hardcoded default at any point in this chain (see
    # docs/ci/HW-IN-LOOP.md).
    port_override = args.serial_port or os.environ.get("ALP_HIL_SERIAL_PORT")
    if port_override:
        specs = [dc.replace(s, serial_port=port_override) for s in specs]
    ssh_host = args.ssh_host or os.environ.get("ALP_HIL_SSH_HOST") or ""
    artifact_dir = str(args.artifact_dir) if args.artifact_dir else ""
    specs = [dc.replace(s, ssh_host=ssh_host, artifact_dir=artifact_dir) for s in specs]

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
    needs_west = any(s.flash_method != SSH_RUN for s in specs)
    if needs_west and not shutil.which("west"):
        print("run_smoke: `west` not on PATH -- HiL runs need the "
              "Zephyr workspace + west.", file=sys.stderr)
        return 2

    results = [run_spec(s) for s in specs]
    return _print_summary(results)


if __name__ == "__main__":
    sys.exit(main())
