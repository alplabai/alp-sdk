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

The store is mounted READ-ONLY (``ro,noatime``): the unit is powered off by cutting the rail,
and a live rw mount would be dirty at every cut. It is remounted rw only while a file is being
cached (then synced and remounted ro), and the tool unmounts it, best effort, before any power
cycle or reboot it drives (steps.unmount_payload_stores).

With ``--create-payload-store`` and no ``alp-payload`` partition, the board appends one itself
(create_partition) to the unpartitioned tail of the boot SD, so the SD never has to go into a
PC; a missing tool, a GPT or odd table, or too little space falls back to the push.

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
            self.t.run(f"mount -o remount,rw {MOUNT}")
        except BenchError as e:
            self.notes.append(f"{local.name}: could not cache in the store ({e})")
            return None
        try:
            if self._cache(local, path, q, sha):
                return path
        finally:                                # back to ro whatever happened above
            self.t.run(f"sync; mount -o remount,ro {MOUNT}", check=False)
        return None

    def _cache(self, local: Path, path: str, q: str, sha: str) -> bool:
        """Transfer ``local`` into the rw-remounted store; False and a note on any failure.
        ConsoleTarget.put leaves ``<dest>.p*`` chunk files and ``<dest>.b64`` beside the
        destination, so a failed push is cleaned of those too."""
        junk = f"{q}.part {q}.part.p* {q}.part.b64"
        try:
            self.t.run(f"mkdir -p {shlex.quote(self.dir)} && rm -f {q} {junk}")
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


def unmount(t) -> None:
    """Best-effort umount before the tool cuts power or reboots; never raises."""
    try:
        t.run(f"sync; umount {MOUNT}", check=False, timeout=30.0)
    except Exception:       # noqa: BLE001  a dead shell must not stop the power cycle
        pass


class NoStore(Exception):
    """The partition cannot be created; the message names why (it becomes payload_store_note)."""


_DUMP = re.compile(r"^(?P<dev>/\S+)\s*:\s*start=\s*(?P<start>\d+),\s*size=\s*(?P<size>\d+),\s*type=(?P<type>\w+)", re.M)
_ALIGN = 2048           # sectors (1 MiB), as prepare_sd


def _have(t, *tools: str) -> str | None:
    """The first of ``tools`` the board has, else None."""
    for tool in tools:
        if t.run(f"command -v {tool}", check=False).rc == 0:
            return tool
    return None


def create_partition(t, disk: str, emmc: str, need_bytes: int) -> str:
    """Append ONE MBR partition labelled alp-payload to ``disk`` (the boot SD) on the board.

    Refuses (NoStore) on the eMMC, a GPT or otherwise unexpected table, no free slot, too
    little unpartitioned space after the last partition, or a missing tool. Returns the new
    partition's device node."""
    if disk.startswith(emmc) or emmc.startswith(disk):
        raise NoStore(f"refusing to partition {disk}: it is the eMMC")
    tools = {"sfdisk": _have(t, "sfdisk"), "partx": _have(t, "partx"), "mke2fs/mkfs.ext4": _have(t, "mkfs.ext4", "mke2fs")}
    if missing := [k for k, v in tools.items() if v is None]:
        raise NoStore(f"cannot create {STORE_LABEL}: the board has no {', '.join(missing)}")
    dump = t.run(f"sfdisk -d {disk}", check=False)
    if dump.rc != 0 or not re.search(r"^label:\s*dos\s*$", dump.stdout, re.M):
        raise NoStore(f"cannot create {STORE_LABEL}: {disk} has no MBR (dos) partition table (GPT or unreadable)")
    parts = [(m["dev"], int(m["start"]), int(m["size"]), m["type"].lower()) for m in _DUMP.finditer(dump.stdout)]
    if (not parts or len(parts) > 3 or any(p[3] in ("5", "f", "85", "ee") for p in parts)
            or any(a[1] + a[2] > b[1] for a, b in zip(parts, parts[1:]))):
        raise NoStore(f"cannot create {STORE_LABEL}: unexpected partition table on {disk}")
    total = t.run(f"cat /sys/block/{disk.rsplit('/', 1)[-1]}/size", check=False).stdout.split()
    if not total or not total[0].isdigit():
        raise NoStore(f"cannot create {STORE_LABEL}: cannot read the size of {disk}")
    start = -(-max(p[1] + p[2] for p in parts) // _ALIGN) * _ALIGN
    sectors = -(-need_bytes // 512 // _ALIGN) * _ALIGN
    if start + sectors > int(total[0]):
        raise NoStore(f"cannot create {STORE_LABEL}: {disk} has {max(int(total[0]) - start, 0) * 512 >> 20} MiB "
                      f"unpartitioned after the last partition, need {sectors * 512 >> 20} MiB")
    t.run(f"echo 'start={start}, size={sectors}, type=83' | sfdisk --no-reread --append {disk}", timeout=60.0)
    t.run(f"partx -a {disk}", check=False)          # a partition is mounted: BLKRRPART would be EBUSY
    after = t.run(f"sfdisk -d {disk}")
    dev = next((m["dev"] for m in _DUMP.finditer(after.stdout) if int(m["start"]) == start), None)
    if dev is None:
        raise NoStore(f"cannot create {STORE_LABEL}: the new partition is not in the table of {disk}")
    if t.run(f"test -b {dev}", check=False).rc != 0:
        raise NoStore(f"cannot create {STORE_LABEL}: the kernel did not add {dev} (partx failed)")
    t.run(f"{tools['mke2fs/mkfs.ext4']} -F -q -t ext4 -L {STORE_LABEL} {dev}", timeout=300.0)
    return dev


def open_store(t, bundle_sha: str, components: list[dict], root: str, emmc: str,
               create_bytes: int | None = None) -> PayloadStore:
    """Mount the SD's ``alp-payload`` partition and return the store for this bundle.
    Any reason it cannot be used yields a store that is off (every file is pushed).
    ``create_bytes`` (--create-payload-store): when the SD has no partition, try to append one
    of at least that size."""
    known = {Path(c["file"]).name: c["sha256"] for c in components if c.get("sha256") and c.get("file")}

    def off(why: str) -> PayloadStore:
        return PayloadStore(t, None, known, f"store off: {why}")

    if root.startswith(emmc):
        return off(f"Linux runs from the eMMC {emmc}, not the provisioning SD")
    disk = re.sub(r"p?\d+$", "", root)
    dev = t.run(f"findfs LABEL={STORE_LABEL}", check=False).stdout.strip()
    created = ""
    if not dev and create_bytes:
        try:
            dev = create_partition(t, disk, emmc, create_bytes)
            created = f"created {STORE_LABEL} on {dev}"
        except (NoStore, BenchError) as e:
            return off(str(e))
    if not dev:
        return off(f"no {STORE_LABEL} partition (run provision_som.py prepare-sd)")
    if not dev.startswith(disk) or dev == root:
        return off(f"{dev} is not on the boot SD {disk}")
    r = t.run(f"mkdir -p {MOUNT} && {{ mountpoint -q {MOUNT} || mount -o ro,noatime {dev} {MOUNT}; }}", check=False)
    if r.rc != 0:
        return off(f"cannot mount {dev}: {r.stdout.strip()[-120:] or r.stderr.strip()[-120:]}")
    return PayloadStore(t, f"{MOUNT}/{bundle_sha}", known, created)
