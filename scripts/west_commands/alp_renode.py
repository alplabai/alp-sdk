# SPDX-License-Identifier: Apache-2.0
"""
`west alp-renode` -- boot the system manifest in Renode for a
heterogeneous smoke test.

Phase 2 ships a stub; the real Renode invocation lands in Phase 3
(pr-renode-dual-os.yml) once the per-SoM .resc scripts are ready.
The stub is wired so customers / CI can call the command today and
get a deterministic exit code + a clear message about when it
will be filled in.

Customer flow (post-Phase-3):

    west alp-build examples/rpmsg-v2n
    west alp-renode examples/rpmsg-v2n      # boots in Renode
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from west import log                            # type: ignore[import-not-found]
from west.commands import WestCommand           # type: ignore[import-not-found]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root           # noqa: E402


class AlpRenode(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-renode",
            "Boot the system manifest in Renode (Phase 3 stub)",
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
        return parser

    def do_run(self, args, _unknown):        # type: ignore[no-untyped-def]
        if find_sdk_root() is None:
            log.die("Cannot locate alp-sdk root.")
            return 1

        app_path = Path(args.app_path).resolve()
        build_root = (Path(args.build_root).resolve()
                      if args.build_root
                      else app_path / "build")
        log.inf(f"alp-renode: would boot {build_root}/system-manifest.yaml")
        log.inf("alp-renode: Renode integration arrives in Phase 3 "
                "(pr-renode-dual-os.yml).  See docs/superpowers/specs/"
                "2026-05-15-heterogeneous-os-orchestration-design.md "
                "§7 phase 3.")
        return 0
