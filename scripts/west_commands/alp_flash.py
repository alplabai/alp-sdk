# SPDX-License-Identifier: Apache-2.0
"""
`west alp-flash` -- walk system-manifest.yaml and program every
slice + helper MCU onto attached hardware in the order dictated by
``boot_order:``.

Wave 5B (2026-05-15) replaces the Phase-2 stub backends with real
subprocess invocations sourced from ``scripts/flash_backends/``.
Every slice's ``flash_method`` + ``flash_args`` is looked up against
the backend registry; backends self-register on import.

Customer flow:

    west alp-build examples/multicore/rpmsg-v2n
    west alp-image examples/multicore/rpmsg-v2n     # optional: pre-build bundle
    west alp-flash examples/multicore/rpmsg-v2n     # respects boot_order
    west alp-flash examples/multicore/rpmsg-v2n --dry-run            # just print
    west alp-flash examples/multicore/rpmsg-v2n --core m33_sm        # one slice
    west alp-flash examples/multicore/rpmsg-v2n --helper gd32_bridge # one helper

When a required tool is missing, the default behaviour is to fail the
slice; pass ``--skip-missing-tools`` to convert those into warnings.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any, Optional

try:
    import yaml                              # type: ignore[import-untyped]
except ImportError:                          # pragma: no cover
    sys.exit("alp-flash: PyYAML is required.  Install via "
             "`pip install pyyaml`.")

try:
    from west import log                     # type: ignore[import-not-found]
    from west.commands import WestCommand    # type: ignore[import-not-found]
    _HAS_WEST = True
except ImportError:                          # pragma: no cover
    # Allow `python alp_flash.py --help` outside a west workspace
    # (e.g. for unit tests / manifest validation).
    _HAS_WEST = False

    class _StubLog:
        @staticmethod
        def inf(msg: str) -> None: print(msg)
        @staticmethod
        def wrn(msg: str) -> None: print(f"WARN: {msg}", file=sys.stderr)
        @staticmethod
        def err(msg: str) -> None: print(f"ERROR: {msg}", file=sys.stderr)
        @staticmethod
        def die(msg: str) -> None:
            print(f"FATAL: {msg}", file=sys.stderr)
            sys.exit(1)

    log = _StubLog()                         # type: ignore[assignment]

    class WestCommand:                       # type: ignore[no-redef]
        """Stand-in so the module imports cleanly without west."""
        name: str = ""
        help: str = ""
        description: str = ""

        def __init__(self, *a, **kw) -> None: pass


sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root        # noqa: E402

# Add scripts/ to sys.path so `import flash_backends` resolves the
# package regardless of how west loaded this wrapper.
_SCRIPTS = Path(__file__).resolve().parents[1]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

import flash_backends                          # noqa: E402
from flash_backends import FlashContext        # noqa: E402


class AlpFlash(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-flash",
            "Walk system-manifest.yaml and program every slice + helper",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):     # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        _add_arguments(parser)
        return parser

    def do_run(self, args, _unknown):         # type: ignore[no-untyped-def]
        return run(args)


def _add_arguments(parser: argparse.ArgumentParser) -> None:
    """Shared argparse wiring; used by both WestCommand and the
    ``python alp_flash.py ...`` standalone path."""
    parser.add_argument(
        "app_path",
        help="Path to the application source directory.")
    parser.add_argument(
        "--build-root", default=None,
        help="Override the build root (default: <app_path>/build).")
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Print the flash commands but don't run them.")
    parser.add_argument(
        "--core", default=None,
        help="Flash only the slice with this core_id (skip every other).")
    parser.add_argument(
        "--helper", default=None,
        help="Flash only the helper MCU with this name (skip slices + "
             "every other helper).")
    parser.add_argument(
        "--skip-missing-tools", action="store_true",
        help="When a backend's required tool is missing, warn + skip "
             "the slice rather than failing the whole flow.")


# ---------------------------------------------------------------------
# Core dispatch (testable: takes a parsed-args object, returns int rc)
# ---------------------------------------------------------------------


def run(args) -> int:                         # type: ignore[no-untyped-def]
    """Real entry point.  Separated from do_run so unit tests can drive
    the dispatcher with a synthetic argparse Namespace."""
    if find_sdk_root() is None:
        log.die("Cannot locate alp-sdk root.")
        return 1

    app_path = Path(args.app_path).resolve()
    build_root = (Path(args.build_root).resolve()
                  if args.build_root
                  else app_path / "build")
    manifest_path = build_root / "system-manifest.yaml"
    if not manifest_path.is_file():
        log.die(f"system-manifest.yaml not found at {manifest_path}; "
                f"run `west alp-build {args.app_path}` first.")
        return 1

    try:
        manifest = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        log.die(f"system-manifest.yaml at {manifest_path} failed to "
                f"parse: {e}")
        return 1
    if not isinstance(manifest, dict):
        log.die(f"system-manifest.yaml at {manifest_path} did not parse "
                f"to a mapping.")
        return 1

    return dispatch(manifest, build_root, args)


def dispatch(manifest: dict[str, Any],
             build_root: Path,
             args) -> int:                    # type: ignore[no-untyped-def]
    """Walk the manifest + dispatch each entry to its registered backend.

    Returns the number of FAILED entries (0 on success).  Skipped
    entries (``--core`` filter, ``--skip-missing-tools`` skips, missing
    flash_method) are not counted as failures.
    """
    sku = (manifest.get("hw_info") or {}).get("sku") or ""

    # Build a slice lookup by core_id + a helper lookup by name.
    slices_by_core: dict[str, dict[str, Any]] = {
        s.get("core_id"): s
        for s in (manifest.get("slices") or [])
        if isinstance(s, dict) and s.get("core_id")
    }
    helpers_by_name: dict[str, dict[str, Any]] = {
        h.get("name"): h
        for h in (manifest.get("helper_mcus") or [])
        if isinstance(h, dict) and h.get("name")
    }

    # Boot order drives the slice walk.  Helpers come after slices
    # (they typically depend on the supervisor SoC being available).
    boot_order = list(manifest.get("boot_order") or [])
    if not boot_order:
        steps = [{"core": cid}
                 for cid in sorted(slices_by_core.keys())]
    else:
        steps = boot_order

    failed = 0
    flashed_anything = False

    # ---- slices ----
    if not args.helper:                       # --helper skips all slices
        for step in steps:
            core = step.get("core") if isinstance(step, dict) else None
            if not core:
                continue
            if args.core and core != args.core:
                continue
            slice_ = slices_by_core.get(core)
            if slice_ is None:
                log.wrn(f"alp-flash: boot_order references core '{core}' "
                        f"not in slices; skipping")
                continue
            rc = _flash_entry(slice_, kind="slice", id_=core,
                              sku=sku, args=args, build_root=build_root)
            if rc < 0:
                continue                       # skipped
            flashed_anything = True
            if rc != 0:
                failed += 1

    # ---- helper MCUs ----
    if not args.core:                         # --core skips all helpers
        for name, helper in helpers_by_name.items():
            if args.helper and name != args.helper:
                continue
            rc = _flash_entry(helper, kind="helper", id_=name,
                              sku=sku, args=args, build_root=build_root)
            if rc < 0:
                continue                       # skipped
            flashed_anything = True
            if rc != 0:
                failed += 1

    if not flashed_anything:
        log.inf("alp-flash: nothing matched the requested filters.")

    log.inf(f"alp-flash: {failed} failure(s).")
    return 1 if failed else 0


def _flash_entry(entry: dict[str, Any],
                 kind: str,
                 id_: str,
                 sku: str,
                 args,                        # type: ignore[no-untyped-def]
                 build_root: Path) -> int:
    """Look up the backend for ``entry`` + invoke it.

    Returns:
       0   success (or clean dry-run / clean skip via --skip-missing-tools)
      -1   silently skipped (no flash_method, --skip-missing-tools etc.)
      >0   backend invocation failed
    """
    method = entry.get("flash_method")
    flash_args = entry.get("flash_args") or {}
    if not method:
        log.inf(f"alp-flash: {kind} '{id_}' has no flash_method; skipping")
        return -1

    backend = flash_backends.lookup(method)
    if backend is None:
        log.err(f"alp-flash: {kind} '{id_}' uses flash_method "
                f"'{method}' which has no registered backend.  "
                f"Available: {sorted(flash_backends.REGISTRY.keys())}")
        return 1

    # Resolve the artefact path.  Slices: output_artefact key.
    # Helpers: firmware_path key (relative to build_root or sdk_root).
    artefact_str = entry.get("output_artefact") or entry.get("firmware_path")
    if not artefact_str:
        # In dry-run we still want to show what we'd do; substitute a
        # placeholder rather than refuse outright.
        if args.dry_run:
            artefact_str = f"<missing-artefact-for-{id_}>"
        else:
            log.err(f"alp-flash: {kind} '{id_}' has no output_artefact / "
                    f"firmware_path; can't flash.")
            return 1
    artefact_path = Path(artefact_str)
    if not artefact_path.is_absolute():
        # Try build_root first (slice artefacts), then sdk root
        # (helper firmware paths are usually repo-relative).
        cand_build = (build_root / artefact_str).resolve()
        sdk_root = find_sdk_root()
        cand_sdk = (sdk_root / artefact_str).resolve() if sdk_root else None
        if cand_build.is_file() or not cand_sdk:
            artefact_path = cand_build
        elif cand_sdk and cand_sdk.is_file():
            artefact_path = cand_sdk
        else:
            artefact_path = cand_build

    # Required-tool gate.  Backends list >=1 candidate; we consider
    # the backend usable when AT LEAST ONE is on PATH (matches the
    # cc3501e_usb_bootloader / swd_probe "either-or" shape).
    requires: list[str] = list(getattr(backend, "requires", []) or [])
    if requires:
        present = [t for t in requires if shutil.which(t)]
        if not present and not args.dry_run:
            msg = (f"alp-flash: {kind} '{id_}' backend '{method}' needs "
                   f"one of {requires} on PATH; none found.")
            if args.skip_missing_tools:
                log.wrn(msg + " (skipped via --skip-missing-tools)")
                return -1
            log.err(msg)
            return 1

    ctx = FlashContext(
        artefact_path=artefact_path,
        flash_args=dict(flash_args),
        core_id=id_,
        sku=sku,
        sdk_root=find_sdk_root(),
        dry_run=bool(args.dry_run),
    )

    log.inf(f"alp-flash: {kind} '{id_}' -> backend '{method}'")
    try:
        result = backend.flash(ctx)
    except NotImplementedError as e:
        log.wrn(f"alp-flash: {kind} '{id_}' backend not yet implemented: "
                f"{e}")
        return -1
    except (OSError, RuntimeError) as e:      # pragma: no cover
        log.err(f"alp-flash: {kind} '{id_}' backend raised: {e}")
        return 1

    if result.ok:
        log.inf(f"  ok ({result.elapsed_s:.1f}s): {result.message}")
        return 0
    log.err(f"  FAIL: {result.message}")
    return 1


# ---------------------------------------------------------------------
# Standalone CLI entry (`python alp_flash.py <app> --dry-run`)
# ---------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    """Standalone entry -- mainly for tests + manifest validation
    outside a west workspace.  When invoked under west, the
    AlpFlash.do_run path is used instead."""
    parser = argparse.ArgumentParser(
        prog="alp-flash",
        description=("Walk system-manifest.yaml and program every slice "
                     "+ helper MCU."),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_arguments(parser)
    args = parser.parse_args(argv)
    return run(args)


if __name__ == "__main__":                    # pragma: no cover
    raise SystemExit(main())
