# SPDX-License-Identifier: Apache-2.0
"""
`west alp-renode` -- boot a built system manifest in Renode for a
heterogeneous (or single-core) smoke test, no hardware required.

Phase-3 slice: the first real target is a single Cortex-M55 Zephyr
slice on the Alif Ensemble E8 (E1M-AEN801).  The command:

  1. reads `<build_root>/system-manifest.yaml` (produced by
     `west alp-build`),
  2. resolves the single `os: zephyr` slice's
     `<build_dir>/zephyr/zephyr.elf`,
  3. maps the SoM family -> a Renode platform descriptor under
     `metadata/renode/<stem>.repl` + `<stem>.resc`,
  4. invokes `renode` headless with a wall-clock timeout, tee-ing the
     UART console output to `--log`.

Customer flow:

    west alp-build examples/peripheral-io/hello-world
    west alp-renode examples/peripheral-io/hello-world --log out.log
    grep -q "[hello] done" out.log

If the `renode` binary is absent the command exits non-zero with a
clear message (it never silently passes).

This module is import-safe WITHOUT west installed (the west imports are
guarded) so the deterministic helpers below can be unit-tested directly
-- see tests/scripts/test_alp_renode.py.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover - surfaced at runtime, not in unit tests
    yaml = None  # type: ignore[assignment]

try:
    from west import log                          # type: ignore[import-not-found]
    from west.commands import WestCommand          # type: ignore[import-not-found]
    _HAVE_WEST = True
except ImportError:  # pragma: no cover - unit tests run without west installed
    _HAVE_WEST = False

    class WestCommand:  # type: ignore[no-redef]
        """Minimal shim so this module imports without west (unit tests)."""

        def __init__(self, *args, **kwargs):  # noqa: D401,ANN002,ANN003
            pass

    log = None  # type: ignore[assignment]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root            # noqa: E402


# Default wall-clock cap for the Renode run, in seconds.  hello-world
# prints its 5 ticks + "[hello] done" within ~6 s of boot; the rest is
# slack for slower CI runners.  Overridable with --timeout.
_DEFAULT_TIMEOUT_S = 120


class AlpRenodeError(Exception):
    """Raised for any deterministic pre-flight failure (bad/missing
    manifest, no zephyr slice, unmapped SoM family, missing descriptor,
    missing `renode` binary).  do_run() converts it into log.die()."""


# ---------------------------------------------------------------------
# Deterministic helpers (unit-tested without renode / west)
# ---------------------------------------------------------------------


# SoM-family token (alif_ensemble / renesas_rzv2n / ...) -> Renode
# platform-descriptor stem under metadata/renode/<stem>.repl + .resc.
# The token itself is computed by mirroring the soc-family-token logic
# in scripts/alp_project.py (`_sku_family` + `_SOC_FAMILY_TOKEN`); this
# table is the second hop, token -> descriptor stem.  Only the Alif
# Ensemble E8 descriptor exists today; other families raise a clear
# error until their .repl/.resc land.
_FAMILY_TOKEN_TO_PLATFORM: dict[str, str] = {
    "alif_ensemble": "alif_ensemble_e8",
}


def platform_stem_for_sku(sku: str) -> str:
    """Map a SoM SKU (e.g. ``E1M-AEN801``) to its Renode platform stem
    (e.g. ``alif_ensemble_e8``).

    Mirrors the family-token logic at scripts/alp_project.py: SKU ->
    family (``aen``) via `_sku_family`, family -> soc token
    (``alif_ensemble``) via `_SOC_FAMILY_TOKEN`, then token -> platform
    stem via `_FAMILY_TOKEN_TO_PLATFORM`.

    Raises AlpRenodeError when the family has no Renode descriptor yet.
    """
    # Lazy import: alp_project pulls in yaml/jsonschema and needs the
    # SDK root on sys.path.  Keeping it lazy lets this module import in
    # contexts where alp_project isn't importable.
    from alp_project import _SOC_FAMILY_TOKEN, _sku_family

    try:
        family = _sku_family(sku)
    except ValueError as e:
        raise AlpRenodeError(str(e)) from e
    token = _SOC_FAMILY_TOKEN.get(family)
    stem = _FAMILY_TOKEN_TO_PLATFORM.get(token) if token else None
    if stem is None:
        raise AlpRenodeError(
            f"no Renode platform descriptor for SoM family '{family}' "
            f"(token={token!r}) of SKU {sku}; wired families: "
            f"{sorted(_FAMILY_TOKEN_TO_PLATFORM)}.  Add "
            f"metadata/renode/<stem>.repl + .resc and a "
            f"_FAMILY_TOKEN_TO_PLATFORM entry to extend coverage.")
    return stem


def platform_files_for_sku(
    sku: str,
    sdk_root: Path,
) -> tuple[Path, Path]:
    """Return ``(repl_path, resc_path)`` under
    ``<sdk_root>/metadata/renode/`` for the SKU's family.  Does not
    check existence -- the caller validates."""
    stem = platform_stem_for_sku(sku)
    base = Path(sdk_root) / "metadata" / "renode"
    return base / f"{stem}.repl", base / f"{stem}.resc"


def load_manifest(build_root: Path) -> dict:
    """Load ``<build_root>/system-manifest.yaml`` into a dict.

    Raises AlpRenodeError when the file is missing or doesn't parse to a
    mapping.
    """
    if yaml is None:  # pragma: no cover - dependency guard
        raise AlpRenodeError(
            "PyYAML is required to read system-manifest.yaml "
            "(pip install pyyaml).")
    mpath = Path(build_root) / "system-manifest.yaml"
    if not mpath.is_file():
        raise AlpRenodeError(
            f"no system-manifest.yaml at {mpath}; run "
            f"`west alp-build <app>` first.")
    data = yaml.safe_load(mpath.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AlpRenodeError(
            f"{mpath} did not parse to a top-level mapping.")
    return data


def zephyr_elf_from_manifest(
    manifest: dict,
    build_root: Path,
) -> Path:
    """Resolve the single Zephyr slice's ``zephyr.elf`` from a parsed
    manifest.

    The Phase-3 smoke supports a single-Zephyr-slice system (one
    Cortex-M core).  Blocked/skipped slices are ignored; if more than
    one runnable Zephyr slice remains, that's an error (the dual-OS
    multi-slice boot is a separate, later target).
    """
    build_root = Path(build_root)
    slices = manifest.get("slices") or []
    zephyr = [
        s for s in slices
        if isinstance(s, dict) and s.get("os") == "zephyr"
    ]
    runnable = [
        s for s in zephyr
        if s.get("status") not in ("blocked", "skipped")
    ]
    pool = runnable or zephyr
    if not pool:
        raise AlpRenodeError(
            "system-manifest.yaml has no os: zephyr slice to boot in "
            "Renode.")
    if len(pool) > 1:
        cores = [s.get("core_id") for s in pool]
        raise AlpRenodeError(
            f"system-manifest.yaml has {len(pool)} zephyr slices "
            f"(cores {cores}); the Phase-3 Renode smoke boots a "
            f"single-Zephyr-slice system.  Multi-slice dual-OS boot is "
            f"a separate target.")
    s = pool[0]
    build_dir = s.get("build_dir")
    if build_dir:
        p = Path(build_dir)
        if not p.is_absolute():
            p = build_root / p
    else:
        p = build_root / f"{s.get('core_id')}-{s.get('os')}"
    return p / "zephyr" / "zephyr.elf"


def resolve_renode_binary(
    which: Callable[[str], Optional[str]] = shutil.which,
) -> str:
    """Return the path to the ``renode`` executable, or raise
    AlpRenodeError when it's not on PATH (the explicit non-zero exit
    path -- never a silent pass).

    The `which` injection point keeps this unit-testable without an
    actual Renode install.
    """
    exe = which("renode")
    if exe is None:
        raise AlpRenodeError(
            "`renode` binary not found on PATH.  Install Renode "
            "(https://renode.io) -- the advisory CI gate "
            ".github/workflows/pr-renode-aen-smoke.yml installs the "
            "pinned v1.15.3 .deb.  `west alp-renode` does not silently "
            "pass when Renode is missing.")
    return exe


def build_renode_argv(
    renode_bin: str,
    repl: Path,
    resc: Path,
    elf: Path,
) -> list[str]:
    """Construct the headless Renode command line.

    Injects the .resc's `$repl` / `$elf` variables on the command line
    (so the static .resc stays generic) and includes the script.
    Headless flags: `--console` (no GUI monitor window), `--disable-xwt`
    (no X), `--hide-monitor` (don't echo the monitor prompt), `--plain`
    (no ANSI control codes -- keeps the tee'd --log greppable).
    """
    return [
        renode_bin,
        "--console",
        "--disable-xwt",
        "--hide-monitor",
        "--plain",
        "-e", f"$repl=@{repl}",
        "-e", f"$elf=@{elf}",
        "-e", f"i @{resc}",
    ]


def _run_renode(
    argv: list[str],
    log_path: Path,
    timeout_s: int,
    expect: Optional[str] = None,
) -> int:
    """Run Renode, tee-ing its (UART + monitor) stdout to ``log_path``.

    Terminates when either: the optional `expect` marker appears in a
    line, the child exits, or `timeout_s` elapses.  Returns 0 unless an
    `expect` marker was requested and not seen.
    """
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + timeout_s
    found = False
    proc = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        with open(log_path, "w", encoding="utf-8") as logf:
            assert proc.stdout is not None
            for line in proc.stdout:
                logf.write(line)
                logf.flush()
                sys.stdout.write(line)
                sys.stdout.flush()
                if expect and expect in line:
                    found = True
                    break
                if time.monotonic() > deadline:
                    break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    if expect:
        return 0 if found else 1
    return 0


# ---------------------------------------------------------------------
# west command
# ---------------------------------------------------------------------


class AlpRenode(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-renode",
            "Boot the built system manifest in Renode (headless smoke)",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):    # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument(
            "app_path", nargs="?", default=".",
            help="Path to the application source directory.")
        parser.add_argument(
            "--build-root", default=None,
            help="Override the build root (default: <app_path>/build).")
        parser.add_argument(
            "--board", default=None,
            help="Override the SoM SKU used to pick the Renode platform "
                 "descriptor (default: hw_info.sku from the manifest).")
        parser.add_argument(
            "--image-bundle", default=None,
            help="Directory of pre-built per-slice artefacts (dual-OS "
                 "boot).  Accepted for parity with the dual-OS flow; "
                 "unused by the single-Zephyr-slice smoke.")
        parser.add_argument(
            "--log", default=None,
            help="Tee the Renode UART/console output to this file "
                 "(default: <build_root>/renode.log).")
        parser.add_argument(
            "--timeout", type=int, default=_DEFAULT_TIMEOUT_S,
            help=f"Wall-clock cap for the Renode run, seconds "
                 f"(default: {_DEFAULT_TIMEOUT_S}).")
        parser.add_argument(
            "--expect", default=None,
            help="If set, stop early (exit 0) when this substring "
                 "appears in the console; exit 1 if the run ends "
                 "without it.")
        return parser

    def do_run(self, args, _unknown):        # type: ignore[no-untyped-def]
        sdk_root = find_sdk_root()
        if sdk_root is None:
            log.die("Cannot locate alp-sdk root.")
            return 1

        app_path = Path(args.app_path).resolve()
        build_root = (Path(args.build_root).resolve()
                      if args.build_root
                      else app_path / "build")
        log_path = (Path(args.log).resolve()
                    if args.log
                    else build_root / "renode.log")

        try:
            manifest = load_manifest(build_root)
            sku = args.board or (manifest.get("hw_info") or {}).get("sku")
            if not sku:
                raise AlpRenodeError(
                    "could not determine SoM SKU: manifest has no "
                    "hw_info.sku and --board was not given.")
            elf = zephyr_elf_from_manifest(manifest, build_root)
            if not elf.is_file():
                raise AlpRenodeError(
                    f"Zephyr ELF not found at {elf}; run "
                    f"`west alp-build {app_path}` first.")
            repl, resc = platform_files_for_sku(sku, sdk_root)
            for descriptor in (repl, resc):
                if not descriptor.is_file():
                    raise AlpRenodeError(
                        f"missing Renode descriptor {descriptor}.")
            renode_bin = resolve_renode_binary()
        except AlpRenodeError as e:
            log.die(str(e))
            return 1

        if args.image_bundle:
            log.inf(f"alp-renode: --image-bundle {args.image_bundle} "
                    f"accepted but unused by the single-slice smoke.")

        log.inf(f"alp-renode: booting {elf} on {repl.name} "
                f"(log -> {log_path})")
        argv = build_renode_argv(renode_bin, repl, resc, elf)
        rc = _run_renode(argv, log_path, args.timeout, expect=args.expect)
        if rc != 0:
            log.die(f"alp-renode: console did not contain "
                    f"{args.expect!r} within {args.timeout}s "
                    f"(see {log_path}).")
        return rc
