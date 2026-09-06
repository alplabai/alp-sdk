# scripts/alp_model/zoo.py
"""Model-zoo machinery: load curated entries, filter by SoM, fetch sources.

Link + fetch + layer: an entry LINKS an upstream source ({url, sha256}) or a
repo-bundled starter ({bundled}) and adds SoM value (validated_soms, compile).
We ship the SoM knowledge, not other people's weights."""
from __future__ import annotations

import hashlib
import shutil
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import yaml


class ZooError(Exception):
    """A zoo entry could not be loaded or its source could not be fetched
    (missing bundled file, download failure, or SHA-256 mismatch)."""


@dataclass(frozen=True)
class ZooEntry:
    id: str
    task: str
    description: str
    license: str
    source: dict
    validated_soms: list[str]
    compile: dict | None
    raw: dict


def load_zoo(metadata_root: Path) -> list[ZooEntry]:
    entries: list[ZooEntry] = []
    for path in sorted((metadata_root / "model_zoo").glob("*.yaml")):
        d = yaml.safe_load(path.read_text(encoding="utf-8"))
        entries.append(ZooEntry(
            id=d["id"], task=d["task"], description=d["description"],
            license=d["license"], source=d["source"],
            validated_soms=list(d.get("validated_soms", [])),
            compile=d.get("compile"), raw=d))
    return entries


def filter_by_sku(entries: list[ZooEntry], sku: str) -> list[ZooEntry]:
    return [e for e in entries if sku in e.validated_soms]


def _suffix_for(entry: ZooEntry) -> str:
    ref = entry.source.get("bundled") or entry.source.get("url", "")
    return Path(ref).suffix or ".model"


def fetch_source(entry: ZooEntry, dest_dir: Path, *, metadata_root: Path) -> Path:
    """Materialise the entry's source into dest_dir, returning the written path.
    Bundled → copy from metadata/model_zoo/. URL → download to a temp file,
    verify sha256, then move into place (no partial file on failure)."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    out = dest_dir / f"{entry.id}{_suffix_for(entry)}"
    src = entry.source
    if "bundled" in src:
        bundled = metadata_root / "model_zoo" / src["bundled"]
        if not bundled.is_file():
            raise ZooError(f"bundled starter not found: {bundled}")
        shutil.copyfile(bundled, out)
        return out
    # url + sha256
    url, want = src["url"], src["sha256"]
    if not (url.startswith("https://") or url.startswith("file://")):
        raise ZooError(f"unsupported source url scheme: {url}")
    tmp = dest_dir / f".{entry.id}.partial"
    try:
        with urllib.request.urlopen(url) as resp:  # noqa: S310 (url validated by schema)
            data = resp.read()
    except OSError as exc:
        raise ZooError(f"download failed for {entry.id}: {exc}") from exc
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        raise ZooError(f"sha256 mismatch for {entry.id}: got {got}, want {want}")
    tmp.write_bytes(data)
    tmp.replace(out)
    return out
