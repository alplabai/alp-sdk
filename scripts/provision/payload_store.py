# SPDX-License-Identifier: Apache-2.0
"""The provisioning SD's payload store: cache bundle payloads on the board, not over a wire.

The store is a directory, ``/var/lib/alp-payload/<bundle sha256>/``, on the SD's root
filesystem. The first unit pushes each file once (console or SSH); every later unit finds it
there and reads it locally instead of receiving the GD32 images over the 115200-baud console or
the 210 MB wic.gz over SSH. The provisioning SD carries license-gated DEEPX files and never
ships with a unit.

Trust: the SD is never trusted. Every use re-hashes the file ON THE BOARD (sha256sum, md5 when
the image's busybox lacks it) against the host's copy, which is itself checked against the signed
bundle.json's sha256 for bundle files. A miss or a mismatch falls back to the wire transfer and
re-caches the file (after a free-space check).

The store is only opened when the board runs from the SD: on an eMMC root it would write into
the unit being provisioned.
"""

from __future__ import annotations

import hashlib
import shlex
from functools import lru_cache
from pathlib import Path

from provision.bench import BenchError

STORE_ROOT = "/var/lib/alp-payload"


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
        if self._cache(local, path, q, sha):
            return path
        return None

    def _free_kib(self) -> int | None:
        """Free KiB on the store's filesystem (``df -k``: header line, then the data line)."""
        out = self.t.run(f"df -k {shlex.quote(self.dir)}", check=False).stdout.splitlines()
        cols = " ".join(out[1:]).split()
        return int(cols[3]) if len(cols) > 3 and cols[3].isdigit() else None

    def _cache(self, local: Path, path: str, q: str, sha: str) -> bool:
        """Transfer ``local`` into the store; False and a note on any failure.
        ConsoleTarget.put leaves ``<dest>.p*`` chunk files and ``<dest>.b64`` beside the
        destination, so a failed push is cleaned of those too."""
        junk = f"{q}.part {q}.part.p* {q}.part.b64"
        try:
            self.t.run(f"mkdir -p {shlex.quote(self.dir)} && rm -f {q} {junk}")
            size = local.stat().st_size
            need = size + size // 5 + (64 << 20)
            free = self._free_kib()
            if free is None or free * 1024 < need:
                self.notes.append(f"{local.name}: not cached, {'unknown' if free is None else free >> 10} "
                                  f"MiB free, need {need >> 20} MiB")
                return False
            self.t.put(local, f"{path}.part")
            self.t.run(f"mv {q}.part {q} && sync")
            if self._good(path, local, sha):
                return True
            self.notes.append(f"{local.name}: cached copy failed its hash check")
        except BenchError as e:
            self.notes.append(f"{local.name}: could not cache in the store ({e})")
        self.t.run(f"rm -f {q} {junk}", check=False)
        return False

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
        a, b = shlex.quote(p), shlex.quote(remote)
        r = t.run(f"cp -f {a} {b} && cmp -s {a} {b}", check=False)
        if r.rc == 127 and t.md5(p) == t.md5(remote):      # no cmp applet: compare md5s instead
            return
        if r.rc != 0:
            raise BenchError(f"copy {p} -> {remote} failed or differs (rc={r.rc}): {r.stdout.strip()[-200:]}")
    else:
        t.put(local, remote)


def open_store(t, bundle_sha: str, components: list[dict], root: str, emmc: str) -> PayloadStore:
    """The store for this bundle, or one that is off (every file is pushed) when Linux runs
    from the eMMC."""
    known = {Path(c["file"]).name: c["sha256"] for c in components if c.get("sha256") and c.get("file")}
    if root.startswith(emmc):
        return PayloadStore(t, None, known, f"store off: Linux runs from the eMMC {emmc}, not the provisioning SD")
    return PayloadStore(t, f"{STORE_ROOT}/{bundle_sha}", known)
