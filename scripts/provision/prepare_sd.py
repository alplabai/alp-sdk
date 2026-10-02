# SPDX-License-Identifier: Apache-2.0
"""``provision_som.py prepare-sd``: build the provisioning SD (release wic + payload store).

1. Check every bundle file against bundle.json's sha256.
2. Write the bundle's ``system_image`` wic to DEVICE (a block device or an image file) -- the
   board still boots Linux from it, U-Boot still finds ``boot/Image`` on p2.
3. Append an MBR partition after the wic's partitions and make it an ext4 filesystem labelled
   ``alp-payload`` (mke2fs -d, no mount, no root for an image file) holding
   ``<bundle sha256>/``: every bundle component, the ``--gd32-fw`` files, a copy of
   bundle.json (and its ``bundle.json.*`` signature siblings) and ``manifest.sha256``.

Needs a Linux userland (WSL on the Windows bench host: ``wsl --mount \\\\.\\PHYSICALDRIVEn
--bare`` exposes the SD as /dev/sdX) because mke2fs (e2fsprogs >= 1.43) is not on Windows.
The partition is a separate one because the wic's rootfs has no guaranteed free space for
~250 MB of payloads. The result carries license-gated DEEPX binaries: INTERNAL ONLY.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from provision.payload_store import STORE_LABEL

ALIGN = 2048          # sectors (1 MiB)


class PrepareError(Exception):
    pass


def _sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def collect(bundle_dir: Path, gd32_fw: Path | None) -> tuple[str, dict[str, Path], dict]:
    """(bundle sha256, {store file name: host path}, bundle). Refuses a file that does not
    match the bundle's sha256, or two different files with one name."""
    bj = bundle_dir / "bundle.json"
    if not bj.is_file():
        raise PrepareError(f"no bundle.json in {bundle_dir}")
    bundle = json.loads(bj.read_text(encoding="utf-8"))
    files: dict[str, Path] = {}

    def add(p: Path) -> None:
        if files.setdefault(p.name, p) != p and _sha256(files[p.name]) != _sha256(p):
            raise PrepareError(f"two different files are both named {p.name}")

    for c in bundle.get("components", []):
        p = bundle_dir / c["file"]
        if not p.is_file():
            raise PrepareError(f"bundle component {c.get('role')}: {p} is missing")
        if c.get("sha256") and _sha256(p) != c["sha256"]:
            raise PrepareError(f"{p} does not match the sha256 in bundle.json")
        add(p)
    if gd32_fw is not None:
        for p in sorted(Path(gd32_fw).iterdir()):
            if p.is_file() and not p.name.startswith("."):
                add(p)
    files["bundle.json"] = bj
    for sib in sorted(bundle_dir.glob("bundle.json.*")):      # signature files, if the release has them
        files[sib.name] = sib
    return _sha256(bj), files, bundle


def _system_image(bundle_dir: Path, bundle: dict) -> Path:
    for c in bundle.get("components", []):
        if c.get("role") == "system_image":
            return bundle_dir / c["file"]
    raise PrepareError("bundle has no system_image component")


def _refuse_mounted(device: Path) -> None:
    try:
        mounts = Path("/proc/mounts").read_text(encoding="utf-8")
    except OSError:
        return
    if any(ln.split()[0].startswith(str(device)) for ln in mounts.splitlines() if ln.startswith("/dev/")):
        raise PrepareError(f"{device} has a mounted partition; unmount it first")


def write_wic(wic_gz: Path, device: Path) -> int:
    """Stream the decompressed wic to DEVICE; returns the byte count."""
    n = 0
    mode = "r+b" if device.exists() else "w+b"
    with gzip.open(wic_gz, "rb") as src, open(device, mode) as dst:
        for block in iter(lambda: src.read(4 << 20), b""):
            dst.write(block)
            n += len(block)
        dst.flush()
        os.fsync(dst.fileno())
    return n


def add_partition(device: Path, size_bytes: int) -> tuple[int, int]:
    """Append an MBR type-0x83 entry after the last partition; returns (start, size) in sectors.
    Grows an image file to fit; refuses a block device that is too small or a GPT wic."""
    with open(device, "r+b") as f:
        mbr = bytearray(f.read(512))
        if mbr[510:512] != b"\x55\xaa":
            raise PrepareError("the written wic has no MBR signature")
        entries = [bytes(mbr[446 + 16 * i:462 + 16 * i]) for i in range(4)]
        if any(e[4] == 0xEE for e in entries):
            raise PrepareError("GPT wic: only an MBR wic is supported (add the partition by hand)")
        slot = next((i for i, e in enumerate(entries) if e[4] == 0), None)
        if slot is None:
            raise PrepareError("the wic's MBR has no free primary slot")
        end = max((struct.unpack_from("<II", e, 8)[0] + struct.unpack_from("<II", e, 8)[1]
                   for e in entries if e[4] != 0), default=ALIGN)
        start = -(-end // ALIGN) * ALIGN
        sectors = -(-size_bytes // 512 // ALIGN) * ALIGN
        f.seek(0, os.SEEK_END)
        have = f.tell()
        if (start + sectors) * 512 > have:
            if device.is_file():
                f.truncate((start + sectors) * 512)
            else:
                raise PrepareError(f"{device} is too small: needs {(start + sectors) * 512} bytes, has {have}")
        mbr[446 + 16 * slot:462 + 16 * slot] = (b"\x00\xfe\xff\xff\x83\xfe\xff\xff"
                                                + struct.pack("<II", start, sectors))
        f.seek(0)
        f.write(mbr)
        f.flush()
        os.fsync(f.fileno())
    return start, sectors


def stage_tree(root: Path, sha: str, files: dict[str, Path]) -> None:
    d = root / sha
    d.mkdir(parents=True)
    lines = []
    for name, p in sorted(files.items()):
        try:
            os.link(p, d / name)
        except OSError:
            shutil.copyfile(p, d / name)
        lines.append(f"{_sha256(p)}  {name}\n")
    (d / "manifest.sha256").write_text("".join(lines), encoding="ascii", newline="\n")


def prepare(bundle_dir: Path, device: Path, gd32_fw: Path | None = None, runner=subprocess.run) -> str:
    sha, files, bundle = collect(bundle_dir, gd32_fw)
    wic = _system_image(bundle_dir, bundle)
    if shutil.which("mke2fs") is None and runner is subprocess.run:
        raise PrepareError("mke2fs (e2fsprogs >= 1.43) not found: run prepare-sd under WSL or Linux")
    _refuse_mounted(device)
    payload = sum(p.stat().st_size for p in files.values())
    n = write_wic(wic, device)
    start, sectors = add_partition(device, payload + payload // 10 + (64 << 20))
    with tempfile.TemporaryDirectory(prefix="alp-payload-") as td:
        stage_tree(Path(td), sha, files)
        r = runner(["mke2fs", "-q", "-F", "-t", "ext4", "-L", STORE_LABEL, "-E", f"offset={start * 512}",
                    "-d", td, str(device), f"{sectors // 2}k"], capture_output=True, text=True)
    if r.returncode != 0:
        raise PrepareError(f"mke2fs failed: {(r.stderr or r.stdout).strip()[-400:]}")
    return (f"wic {n} bytes -> {device}; {STORE_LABEL} partition at sector {start} ({sectors * 512 >> 20} MiB) "
            f"holds {len(files)} files for bundle {sha}")
