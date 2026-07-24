# scripts/alp_cli/model.py
"""`alp model` subcommands: compile + package AI models into .alpmodel."""
from __future__ import annotations
import json
from pathlib import Path

import click
import yaml

from alp_model.build import build_model
from alp_model.package import read_package

_DEFAULT_META = Path(__file__).resolve().parents[2] / "metadata"


def _resolve_compile(block: dict | None, base: Path) -> dict | None:
    """Resolve every path value in each per-backend compile block to an absolute
    path relative to the board.yaml dir (all current opts values are paths)."""
    if not block:
        return None
    return {
        backend: {k: str((base / v).resolve()) if isinstance(v, str) else v
                  for k, v in (opts or {}).items()}
        for backend, opts in block.items()
    }


@click.group(name="model", help="AI model compilation & packaging.")
def model_group() -> None:
    pass


def _target_payload(mft, blobs) -> tuple[list[dict], list[dict]]:
    targets = [{
        "backend": t.backend, "silicon_ref": t.silicon_ref,
        "blob_format": t.blob_format, "accel_config": t.accel_config,
        "arena": t.arena, "blob_bytes": len(blobs[t.blob]),
        "requires": t.requires, "compiler_version": t.compiler_version,
    } for t in mft.targets]
    skipped = [{
        "backend": c.backend, "accel_config": c.accel_config,
        "status": c.status, "reason": c.reason,
    } for c in mft.coverage]
    return targets, skipped


@model_group.command(name="build", help="Compile board.yaml models: into .alpmodel packages.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=Path("board.yaml"), show_default=True, help="Path to board.yaml.")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Output directory.")
@click.option("--metadata-root", type=click.Path(exists=True, path_type=Path),
              default=_DEFAULT_META, help="Path to the metadata/ root.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True,
              help="human: 'built <path>' lines. json: a {models:[...]} payload "
                   "(targets + skipped coverage) for machine consumption.")
def build_cmd(board_path: Path, out_dir: Path, metadata_root: Path,
              output_format: str) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    sku = board["som"]["sku"]
    models = board.get("models", [])
    base = board_path.parent
    if output_format == "human":
        if not models:
            click.echo("no models: declared in board.yaml; nothing to build.")
            return
        for m in models:
            source = (base / m["source"]).resolve()
            out = build_model(sku=sku, name=m["name"], source=source,
                              out_dir=out_dir, metadata_root=metadata_root,
                              compile_opts=_resolve_compile(m.get("compile"), base))
            click.echo(f"built {out}")
        return

    entries: list[dict] = []
    failed = False
    for m in models:
        source = (base / m["source"]).resolve()
        try:
            out = build_model(sku=sku, name=m["name"], source=source,
                              out_dir=out_dir, metadata_root=metadata_root,
                              compile_opts=_resolve_compile(m.get("compile"), base))
        except ValueError as exc:      # build_model raises when no blob compiles
            failed = True
            entries.append({"name": m["name"], "source": str(source),
                            "error": str(exc), "targets": [], "skipped": []})
            continue
        mft, blobs = read_package(out.read_bytes())
        targets, skipped = _target_payload(mft, blobs)
        entries.append({"name": m["name"], "source": str(source),
                        "alpmodel_path": str(out), "total_bytes": out.stat().st_size,
                        "targets": targets, "skipped": skipped})
    click.echo(json.dumps({"models": entries}, indent=2))
    if failed:
        raise SystemExit(1)
