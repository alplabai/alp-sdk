# scripts/alp_cli/model.py
"""`alp model` subcommands: compile + package AI models into .alpmodel."""
from __future__ import annotations
from pathlib import Path

import click
import yaml

from alp_model.build import build_model

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


@model_group.command(name="build", help="Compile board.yaml models: into .alpmodel packages.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=Path("board.yaml"), show_default=True, help="Path to board.yaml.")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Output directory.")
@click.option("--metadata-root", type=click.Path(exists=True, path_type=Path),
              default=_DEFAULT_META, help="Path to the metadata/ root.")
def build_cmd(board_path: Path, out_dir: Path, metadata_root: Path) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    sku = board["som"]["sku"]
    models = board.get("models", [])
    if not models:
        click.echo("no models: declared in board.yaml; nothing to build.")
        return
    base = board_path.parent
    for m in models:
        source = (base / m["source"]).resolve()
        out = build_model(sku=sku, name=m["name"], source=source,
                          out_dir=out_dir, metadata_root=metadata_root,
                          compile_opts=_resolve_compile(m.get("compile"), base))
        click.echo(f"built {out}")
