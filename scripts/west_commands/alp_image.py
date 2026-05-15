# SPDX-License-Identifier: Apache-2.0
"""
`west alp-image` -- read the per-project system-manifest.yaml that
`alp-build` wrote and assemble a single flashable bundle under
<app>/build/image-bundle/.

Per spec §5.2 step 7 + §10.3 ("alp-image bundle format" open
question), Phase 2 emits a tar.gz + bundle-manifest.json pair.
Mender `.swu` bundling lands in Phase 3+ once the OTA bundler API
is finalised.

Customer flow:

    west alp-build examples/rpmsg-v2n      # produces system-manifest
    west alp-image examples/rpmsg-v2n      # produces image-bundle/

The bundle layout:

    build/image-bundle/
    ├── bundle-manifest.json   (this command writes it)
    ├── slices/
    │   ├── a55_cluster-yocto.tar.gz
    │   └── m33_sm-zephyr.tar.gz
    └── helper-mcus/
        ├── gd32_bridge.bin    (when registered)
        └── cc3501e_otp.blob
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
from pathlib import Path

import yaml                                       # type: ignore[import-untyped]
from west import log                              # type: ignore[import-not-found]
from west.commands import WestCommand             # type: ignore[import-not-found]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root             # noqa: E402


class AlpImage(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-image",
            "Assemble a single flashable bundle from system-manifest.yaml",
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
            "app_path",
            help="Path to the application source directory.")
        parser.add_argument(
            "--build-root", default=None,
            help="Override the build root (default: <app_path>/build).")
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
        manifest_path = build_root / "system-manifest.yaml"
        if not manifest_path.is_file():
            log.die(f"system-manifest.yaml not found at {manifest_path}; "
                    f"run `west alp-build {args.app_path}` first.")
            return 1

        manifest = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(manifest, dict):
            log.die(f"system-manifest.yaml at {manifest_path} did not "
                    f"parse to a mapping.")
            return 1

        bundle_dir = build_root / "image-bundle"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        slices_dir = bundle_dir / "slices"
        slices_dir.mkdir(exist_ok=True)
        helpers_dir = bundle_dir / "helper-mcus"
        helpers_dir.mkdir(exist_ok=True)

        bundle_entries: list[dict[str, object]] = []

        for slice_ in manifest.get("slices", []) or []:
            if slice_.get("status") != "ok":
                log.inf(f"alp-image: skipping {slice_.get('core_id')} "
                        f"(status: {slice_.get('status')})")
                continue
            core_id = slice_.get("core_id", "unknown")
            os_kind = slice_.get("os", "unknown")
            build_dir = slice_.get("build_dir")
            if not build_dir or not Path(build_dir).is_dir():
                log.inf(f"alp-image: skipping {core_id} (build_dir missing)")
                continue
            archive = slices_dir / f"{core_id}-{os_kind}.tar.gz"
            self._tar_directory(Path(build_dir), archive)
            bundle_entries.append({
                "core_id":  core_id,
                "os":       os_kind,
                "artefact": str(archive.relative_to(bundle_dir)),
                "sha256":   self._sha256(archive),
                "size":     archive.stat().st_size,
            })

        helper_entries: list[dict[str, object]] = []
        for hm in manifest.get("helper_mcus", []) or []:
            fw = hm.get("firmware_path")
            if not fw:
                continue
            fw_path = Path(fw)
            if not fw_path.is_file():
                log.inf(f"alp-image: helper-mcu firmware not found at "
                        f"{fw_path}; skipping")
                continue
            dst = helpers_dir / fw_path.name
            shutil.copy2(fw_path, dst)
            helper_entries.append({
                "name":     hm.get("name"),
                "role":     hm.get("role"),
                "artefact": str(dst.relative_to(bundle_dir)),
                "sha256":   self._sha256(dst),
                "size":     dst.stat().st_size,
            })

        bundle_manifest = {
            "schema_version": 1,
            "generated_by":   "west alp-image",
            "hw_info":        manifest.get("hw_info", {}),
            "slices":         bundle_entries,
            "helper_mcus":    helper_entries,
            "boot_order":     manifest.get("boot_order", []),
        }
        (bundle_dir / "bundle-manifest.json").write_text(
            json.dumps(bundle_manifest, indent=2), encoding="utf-8")
        log.inf(f"alp-image: bundle ready at {bundle_dir}")
        return 0

    @staticmethod
    def _tar_directory(src: Path, dst: Path) -> None:
        """Tar+gzip `src` into `dst` (overwriting)."""
        if dst.exists():
            dst.unlink()
        with tarfile.open(dst, "w:gz") as tf:
            tf.add(src, arcname=src.name)

    @staticmethod
    def _sha256(path: Path) -> str:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
