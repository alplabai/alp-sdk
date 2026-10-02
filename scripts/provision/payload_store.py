# SPDX-License-Identifier: Apache-2.0
"""The provisioning SD's payload store: read bundle payloads on the board, not over a wire.

``provision_som.py prepare-sd`` writes the release wic to the provisioning SD and appends
an ext4 partition labelled ``alp-payload`` holding ``<bundle sha256>/<file>`` for every
bundle artefact the on-board steps need, plus the GD32 images (see prepare_sd.py). The
board boots Linux from that SD, so a step can read a payload locally instead of
receiving it over the 115200-baud console (GD32 images, no network yet) or SSH
(DX-M1 files, DTBs, the 210 MB wic.gz).

Trust: the SD is never trusted. Every use re-hashes the file ON THE BOARD (sha256sum,
md5 when the image's busybox lacks it) against the host's copy, which is itself checked
against the signed bundle.json's sha256 for bundle files. A miss or a mismatch falls
back to the wire transfer and re-caches the file in the store (lazy self-provisioning:
an empty ``alp-payload`` partition fills itself on the first unit).

The store is only opened when the board runs from the SD: on an eMMC root it would write
into the unit being provisioned.
"""

from __future__ import annotations

import hashlib
import re
import shlex
from functools import lru_cache
from pathlib import Path

from provision.bench import BenchError

STORE_LABEL = "alp-payload"
MOUNT = "/mnt/alp-payload"


@lru_cache(maxsize=64)
def _digest(path: str, mtime_ns: int, size: int, algo: str) -> str:
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def host_digest(p: Path, algo: str = "sha256") -> str:
    st = Path(p).stat()
    return _digest(str(p), st.st_mtime_ns, st.st_size, algo)


class PayloadStore:
    def __init__(self, t, directory: str | None, known: dict[str, str] | None = None,
                 off: str = "") -> None:
        self.t, self.dir, self.off = t, directory, off
        self.known = known or {}              # file name -> sha256 from the signed bundle.json
        self.sources: dict[str, str] = {}     # file name -> "sd-store" | "pushed"
        self.notes: list[str] = [off] if off else []

    def _sha(self, local: Path) -> str:
        got = host_digest(local)
        if (want := self.known.get(local.name)) and want != got:
            raise BenchError(f"{local}: sha256 {got} differs from the signed bundle's {want}")
        return got

    def _good(self, path: str, local: Path, sha: str) -> bool:
        """The file is on the board AND its hash, computed on the board, matches."""
        q = shlex.quote(path)
        if self.t.run(f"test -f {q}", check=False).rc != 0:
            return False
        r = self.t.run(f"sha256sum < {q}", timeout=900.0, check=False)
        if r.rc == 0 and r.stdout.split():
            return r.stdout.split()[0] == sha
        try:                                   # no sha256sum applet: md5 against the host's md5
            return self.t.md5(path) == host_digest(local, "md5")
        except BenchError:
            return False

    def fetch(self, local: Path) -> str | None:
        """On-board path of ``local``'s content, hash-verified; None -> the caller must push."""
        local = Path(local)
        if self.dir is None:
            self.sources[local.name] = "pushed"
            return None
        sha = self._sha(local)
        path, q = f"{self.dir}/{local.name}", shlex.quote(f"{self.dir}/{local.name}")
        if self._good(path, local, sha):
            self.sources[local.name] = "sd-store"
            return path
        # miss or bad hash: transfer once into the store so every later unit finds it
        self.sources[local.name] = "pushed"
        try:
            self.t.run(f"mkdir -p {shlex.quote(self.dir)} && rm -f {q} {q}.part")
            self.t.put(local, f"{path}.part")
            self.t.run(f"mv {q}.part {q} && sync")
            if self._good(path, local, sha):
                return path
            self.notes.append(f"{local.name}: cached copy failed its hash check")
        except BenchError as e:
            self.notes.append(f"{local.name}: could not cache in the store ({e})")
        self.t.run(f"rm -f {q} {q}.part", check=False)
        return None

    def summary(self) -> str:
        return "sd-store" if self.sources and set(self.sources.values()) == {"sd-store"} else "pushed"

    def evidence(self) -> dict[str, str]:
        ev = {"payload_source": self.summary()}
        if self.notes:
            ev["payload_store_note"] = "; ".join(self.notes)[:400]
        return ev


def evidence(store: PayloadStore | None) -> dict[str, str]:
    return store.evidence() if store else {"payload_source": "pushed"}


def stage(t, store: PayloadStore | None, local: Path, remote: str) -> None:
    """Make ``local`` appear at ``remote``: a hash-verified local copy from the store, else scp."""
    if store is not None and (p := store.fetch(local)) is not None:
        t.run(f"cp -f {shlex.quote(p)} {shlex.quote(remote)}")
    else:
        t.put(local, remote)


def open_store(t, bundle_sha: str, components: list[dict], root: str, emmc: str) -> PayloadStore:
    """Mount the SD's ``alp-payload`` partition and return the store for this bundle.
    Any reason it cannot be used yields a store that is off (every file is pushed)."""
    known = {Path(c["file"]).name: c["sha256"] for c in components if c.get("sha256") and c.get("file")}

    def off(why: str) -> PayloadStore:
        return PayloadStore(t, None, known, f"store off: {why}")

    if root.startswith(emmc):
        return off(f"Linux runs from the eMMC {emmc}, not the provisioning SD")
    disk = re.sub(r"p?\d+$", "", root)
    dev = t.run(f"findfs LABEL={STORE_LABEL}", check=False).stdout.strip()
    if not dev:
        return off(f"no {STORE_LABEL} partition (run provision_som.py prepare-sd)")
    if not dev.startswith(disk) or dev == root:
        return off(f"{dev} is not on the boot SD {disk}")
    r = t.run(f"mkdir -p {MOUNT} && {{ mountpoint -q {MOUNT} || mount {dev} {MOUNT}; }}", check=False)
    if r.rc != 0:
        return off(f"cannot mount {dev}: {r.stdout.strip()[-120:] or r.stderr.strip()[-120:]}")
    return PayloadStore(t, f"{MOUNT}/{bundle_sha}", known)
