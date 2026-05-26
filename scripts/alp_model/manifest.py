"""Manifest data model for .alpmodel packages (canonical Python form)."""
from __future__ import annotations
import json as _json
from dataclasses import dataclass, field, asdict

MANIFEST_SCHEMA_VERSION = 1


def _json_default(d: dict) -> dict:
    out = dict(d)
    out["src_sha"] = d["src_sha"].hex()          # bytes -> hex string for JSON
    return out


@dataclass
class Tensor:
    dtype: str
    rank: int
    shape: list[int]
    scale: float
    zp: int


@dataclass
class Target:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # e.g. "alif:ensemble:e8" or "*"
    blob_format: str        # vela_tflite | drpai_dir | dxnn | tflite
    accel_config: str       # "" when N/A
    arena: int
    requires: dict[str, object]  # {"sram_kib": int, "op_features": list[str]}
    blob: int               # index into the package blob table


@dataclass
class Coverage:
    """A backend the package did NOT include a blob for, and why."""

    backend: str
    accel_config: str
    status: str             # compiled | skipped | incompatible
    reason: str


@dataclass
class Manifest:
    name: str
    src_sha: bytes
    inputs: list[Tensor] = field(default_factory=list)
    outputs: list[Tensor] = field(default_factory=list)
    targets: list[Target] = field(default_factory=list)
    coverage: list[Coverage] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "v": MANIFEST_SCHEMA_VERSION,
            "name": self.name,
            "src_sha": self.src_sha,
            "inputs": [asdict(t) for t in self.inputs],
            "outputs": [asdict(t) for t in self.outputs],
            "targets": [asdict(t) for t in self.targets],
            "coverage": [asdict(c) for c in self.coverage],
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Manifest":
        v = d.get("v", MANIFEST_SCHEMA_VERSION)
        if v != MANIFEST_SCHEMA_VERSION:
            raise ValueError(f"unsupported manifest version {v!r}; expected {MANIFEST_SCHEMA_VERSION}")
        return cls(
            name=d["name"],
            src_sha=d["src_sha"],  # raw bytes pass-through; JSON/text callers must decode to bytes first
            inputs=[Tensor(**t) for t in d.get("inputs", [])],
            outputs=[Tensor(**t) for t in d.get("outputs", [])],
            targets=[Target(**t) for t in d.get("targets", [])],
            coverage=[Coverage(**c) for c in d.get("coverage", [])],
        )

    def to_json(self) -> str:
        return _json.dumps(_json_default(self.to_dict()), indent=2)

    @classmethod
    def from_json(cls, text: str) -> "Manifest":
        d = _json.loads(text)
        d["src_sha"] = bytes.fromhex(d["src_sha"])
        return cls.from_dict(d)
