# scripts/alp_cli/model.py
"""`alp model` subcommands: compile + package AI models into .alpmodel."""
from __future__ import annotations
import json
from pathlib import Path

import click
import yaml

from alp_model.build import build_model, _ADAPTERS
from alp_model.package import read_package
from alp_model.targets import resolve_targets

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
        except Exception as exc:       # no blob compiled (ValueError), bad source
                                        # path (OSError), or a real compiler
                                        # failure (RuntimeError) -- all must
                                        # surface as JSON, never a traceback.
            failed = True
            entries.append({"name": m["name"], "source": m["source"],
                            "error": str(exc), "targets": [], "skipped": []})
            continue
        mft, blobs = read_package(out.read_bytes())
        targets, skipped = _target_payload(mft, blobs)
        entries.append({"name": m["name"], "source": m["source"],
                        "alpmodel_path": str(out), "total_bytes": out.stat().st_size,
                        "targets": targets, "skipped": skipped})
    click.echo(json.dumps({"models": entries}, indent=2))
    if failed:
        raise SystemExit(1)


@model_group.command(name="list", help="List board.yaml models: + built .alpmodel status.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=Path("board.yaml"), show_default=True, help="Path to board.yaml.")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Build output directory.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def list_cmd(board_path: Path, out_dir: Path, output_format: str) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    base = board_path.parent
    entries: list[dict] = []
    for m in board.get("models", []):
        source = (base / m["source"]).resolve()
        artifact = out_dir / f"{m['name']}.alpmodel"
        exists = artifact.is_file()
        stale = bool(exists and source.is_file()
                     and source.stat().st_mtime > artifact.stat().st_mtime)
        entries.append({
            "name": m["name"], "source": m["source"], "compile": m.get("compile"),
            "artifact": {"exists": exists, "path": str(artifact.resolve()),
                         "bytes": artifact.stat().st_size if exists else 0,
                         "stale": stale},
        })
    if output_format == "json":
        click.echo(json.dumps({"models": entries}, indent=2))
    else:
        for e in entries:
            a = e["artifact"]
            state = "missing" if not a["exists"] else ("stale" if a["stale"] else "built")
            click.echo(f"{e['name']:20} {state:8} {e['source']}")


@model_group.command(name="info", help="Decode a built .alpmodel: targets, requires, coverage matrix.")
@click.argument("name")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Build output directory.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=None, help="board.yaml — enables the SoM coverage matrix.")
@click.option("--metadata-root", type=click.Path(exists=True, path_type=Path),
              default=_DEFAULT_META, help="Path to the metadata/ root.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def info_cmd(name: str, out_dir: Path, board_path: Path | None,
             metadata_root: Path, output_format: str) -> None:
    artifact = out_dir / f"{name}.alpmodel"
    if not artifact.is_file():
        click.echo(f"alp model info: no .alpmodel for '{name}' at {artifact}", err=True)
        raise SystemExit(1)
    mft, blobs = read_package(artifact.read_bytes())
    doc = json.loads(mft.to_json())        # name/src_sha/inputs/outputs (public API)
    targets, skipped = _target_payload(mft, blobs)
    doc["targets"] = targets
    doc["skipped"] = skipped
    doc.pop("coverage", None)
    doc.pop("v", None)
    if board_path is not None:
        sku = yaml.safe_load(board_path.read_text(encoding="utf-8"))["som"]["sku"]
        have = {t.backend for t in mft.targets}
        declared = {s.backend for s in resolve_targets(sku, metadata_root=metadata_root)}
        doc["coverage_matrix"] = [{"backend": b, "has_blob": b in have}
                                  for b in sorted(declared)]
    if output_format == "json":
        click.echo(json.dumps(doc, indent=2))
    else:
        click.echo(f"{doc['name']}: {len(doc['targets'])} blob(s), "
                   f"{len(doc['skipped'])} skipped")
        for t in doc["targets"]:
            click.echo(f"  {t['backend']:12} {t['blob_format']:12} {t['blob_bytes']} B")


@model_group.command(name="doctor", help="Report installed NPU compiler toolchains.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def doctor_cmd(output_format: str) -> None:
    tools = [a.probe() for a in _ADAPTERS]
    if output_format == "json":
        click.echo(json.dumps({"toolchains": tools}, indent=2))
    else:
        for t in tools:
            mark = "ok" if t["available"] else "--"
            ver = t["version"] or t["reason"] or ""
            click.echo(f"[{mark}] {t['backend']:12} {t['tool']:12} {ver}")
