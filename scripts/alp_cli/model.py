# scripts/alp_cli/model.py
"""`alp model` subcommands: compile + package AI models into .alpmodel."""
from __future__ import annotations
import json
from pathlib import Path

import click
import yaml

from alp_model.analyze import UnsupportedModelError, analyze_model
from alp_model.build import build_model, _ADAPTERS
from alp_model.package import read_package
from alp_model.prep import PrepError, accuracy_delta, quantize, validate_calibration
from alp_model.targets import resolve_targets
from alp_model.zoo import ZooError, fetch_source, load_zoo

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
@click.option("--model", "model_name", default=None,
              help="Build only this model (default: all board.yaml models).")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True,
              help="human: 'built <path>' lines. json: a {models:[...]} payload "
                   "(targets + skipped coverage) for machine consumption.")
def build_cmd(board_path: Path, out_dir: Path, metadata_root: Path,
              model_name: str | None, output_format: str) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    sku = board["som"]["sku"]
    models = board.get("models", [])
    base = board_path.parent
    if model_name is not None:
        models = [m for m in models if m["name"] == model_name]
        if not models:
            if output_format == "json":
                click.echo(json.dumps({"models": []}, indent=2))
            else:
                click.echo(f"alp model build: no model named '{model_name}' in board.yaml",
                           err=True)
            raise SystemExit(1)
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


def _backend_payload(b) -> dict:
    return {
        "backend": b.backend, "verdict": b.verdict,
        "est_sram_kib": b.est_sram_kib, "budget_sram_kib": b.budget_sram_kib,
        "est_latency_ms": b.est_latency_ms,
        "op_coverage_pct": b.op_coverage_pct,
        "unsupported_ops": b.unsupported_ops, "source": b.source,
    }


def _echo_backends_human(backends) -> None:
    for b in backends:
        lat = "n/a" if b.est_latency_ms is None else f"{b.est_latency_ms:.2f} ms"
        budget = "unknown" if b.budget_sram_kib is None else f"{b.budget_sram_kib} KiB"
        click.echo(
            f"  [{b.verdict:>12}] {b.backend:<11} "
            f"sram ~{b.est_sram_kib} KiB / budget {budget}  "
            f"latency {lat}  ops {b.op_coverage_pct:.0f}%  ({b.source})")
        if b.unsupported_ops:
            click.echo(f"               unsupported: {', '.join(b.unsupported_ops)}")


@model_group.command(name="check", help="Static pre-flight fit/perf check for a model "
                      "(or every board.yaml models: entry) on a SoM (offline, no toolchain).")
@click.argument("model", required=False, default=None,
                 type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--sku", default=None,
              help="SoM SKU, e.g. E1M-AEN801. Required unless --board is given (board.yaml "
                   "supplies som.sku); passing --sku together with --board overrides it.")
@click.option("--board", "board_path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
              default=None, help="Check every (or one, with --model) models: entry in this "
                   "board.yaml instead of a single model. Mutually exclusive with the "
                   "positional MODEL.")
@click.option("--model", "select_name", default=None,
              help="With --board, check only the named models: entry.")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False,
              help="Metadata root (defaults to the SDK's metadata/).")
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def check_cmd(model: Path | None, sku: str | None, board_path: Path | None,
              select_name: str | None, metadata_root: Path, fmt: str) -> None:
    if board_path is not None and model is not None:
        click.echo("error: MODEL and --board are mutually exclusive", err=True)
        raise SystemExit(2)

    if board_path is None:
        if model is None or not sku:
            click.echo("error: MODEL and --sku are required unless --board is given", err=True)
            raise SystemExit(2)
        try:
            result = analyze_model(model, sku, metadata_root=metadata_root)
        except (UnsupportedModelError, FileNotFoundError) as exc:
            raise SystemExit(f"error: {exc}")

        if fmt == "json":
            payload = {
                "model": result.model, "sku": result.sku,
                "backends": [_backend_payload(b) for b in result.backends],
                "suggestion": result.suggestion,
            }
            click.echo(json.dumps(payload, indent=2))
            return
        click.echo(f"model: {result.model}")
        click.echo(f"SoM:   {result.sku}")
        _echo_backends_human(result.backends)
        if result.suggestion:
            click.echo(f"suggestion: {result.suggestion}")
        return

    # --board mode: sku from board.som.sku unless --sku overrides it; walk every
    # (or the one named) models: entry through the same engine, per-model.
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    board_sku = sku or board["som"]["sku"]
    base = board_path.parent
    models = board.get("models", [])
    if select_name is not None:
        models = [m for m in models if m["name"] == select_name]
        if not models:
            raise SystemExit(f"error: no model named '{select_name}' in {board_path}")

    entries: list[dict] = []       # {"name","source","result"} or {"name","source","error"}
    failed = False
    for m in models:
        source = (base / m["source"]).resolve()
        try:
            result = analyze_model(source, board_sku, metadata_root=metadata_root)
        except (UnsupportedModelError, FileNotFoundError) as exc:
            failed = True
            entries.append({"name": m["name"], "source": m["source"], "error": str(exc)})
            continue
        entries.append({"name": m["name"], "source": m["source"], "result": result})

    if fmt == "json":
        payload_models = [
            {"name": e["name"], "source": e["source"], "error": e["error"]}
            if "error" in e else
            {"name": e["name"], "source": e["source"],
             "backends": [_backend_payload(b) for b in e["result"].backends],
             "suggestion": e["result"].suggestion}
            for e in entries
        ]
        click.echo(json.dumps(
            {"board": str(board_path), "sku": board_sku, "models": payload_models}, indent=2))
        if failed:
            raise SystemExit(1)
        return

    click.echo(f"board: {board_path}")
    click.echo(f"SoM:   {board_sku}")
    for e in entries:
        click.echo(f"model: {e['name']}")
        if "error" in e:
            click.echo(f"  error: {e['error']}")
            continue
        _echo_backends_human(e["result"].backends)
        if e["result"].suggestion:
            click.echo(f"  suggestion: {e['result'].suggestion}")
    if failed:
        raise SystemExit(1)


@model_group.command(name="zoo", help="Browse curated model-zoo entries (and which run on a SoM).")
@click.option("--sku", default=None, help="Mark which entries run on this SoM (via validated_soms).")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False)
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def zoo_cmd(sku: str | None, metadata_root: Path, fmt: str) -> None:
    entries = load_zoo(metadata_root)
    rows = [{
        "id": e.id, "task": e.task, "description": e.description,
        "license": e.license, "validated_soms": e.validated_soms,
        "runs_here": (sku in e.validated_soms) if sku else None,
    } for e in entries]
    if fmt == "json":
        click.echo(json.dumps({"entries": rows}, indent=2))
        return
    for r in rows:
        mark = "" if r["runs_here"] is None else ("  [runs here]" if r["runs_here"] else "  [not validated here]")
        click.echo(f"{r['id']:<20} {r['task']:<14} {r['description']}{mark}")


@model_group.command(name="add", help="Add a model-zoo entry to board.yaml (fetch source + append models:).")
@click.argument("zoo_id")
@click.option("--board", "board_path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
              default=Path("board.yaml"), show_default=True)
@click.option("--name", default=None, help="models: entry name (default: the zoo id).")
@click.option("--models-dir", "models_dir", default="models",
              help="Directory (relative to board.yaml) to cache the fetched model.")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False)
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def add_cmd(zoo_id: str, board_path: Path, name: str | None, models_dir: str,
            metadata_root: Path, fmt: str) -> None:
    entry = next((e for e in load_zoo(metadata_root) if e.id == zoo_id), None)
    if entry is None:
        click.echo(f"error: no zoo entry '{zoo_id}'", err=True)
        raise SystemExit(1)
    name = name or entry.id
    board = yaml.safe_load(board_path.read_text(encoding="utf-8")) or {}
    models = board.get("models") or []
    if any(m.get("name") == name for m in models):
        click.echo(f"error: board.yaml already has a model named '{name}'", err=True)
        raise SystemExit(1)
    base = board_path.parent
    dest = (base / models_dir).resolve()
    base_resolved = base.resolve()
    if dest != base_resolved and base_resolved not in dest.parents:
        click.echo("error: --models-dir must be inside the board directory", err=True)
        raise SystemExit(1)
    try:
        fetched = fetch_source(entry, base / models_dir, metadata_root=metadata_root)
    except ZooError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    rel = fetched.resolve().relative_to(base.resolve()).as_posix()
    new_entry = {"name": name, "source": rel}
    if entry.compile:
        new_entry["compile"] = entry.compile
    models.append(new_entry)
    board["models"] = models
    board_path.write_text(yaml.safe_dump(board, sort_keys=False), encoding="utf-8")
    result = {"added": name, "source": rel, "from": entry.id}
    click.echo(json.dumps(result, indent=2) if fmt == "json" else f"added '{name}' ({rel}) from zoo '{entry.id}'")


@model_group.command(name="prep", help="License-free INT8 quantize + fp32-vs-int8 accuracy report.")
@click.argument("raw", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--calibration", "cal_dir", required=True,
              type=click.Path(exists=True, file_okay=False, path_type=Path),
              help="Directory of .npy calibration samples matching the model input.")
@click.option("--out", default=None, type=click.Path(path_type=Path),
              help="Output INT8 .onnx (default: <raw>.int8.onnx).")
@click.option("--per-channel", is_flag=True, help="Per-channel weight quantization (often recovers accuracy).")
@click.option("--min-samples", default=8, show_default=True, help="Minimum calibration samples.")
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def prep_cmd(raw: Path, cal_dir: Path, out: Path | None, per_channel: bool,
             min_samples: int, fmt: str) -> None:
    if raw.suffix.lower() != ".onnx":
        click.echo(f"error: model prep supports .onnx input in this release; got {raw.name}",
                   err=True)
        raise SystemExit(1)
    out = out or raw.with_suffix(".int8.onnx")
    try:
        validate_calibration(cal_dir, raw, min_samples=min_samples)
        quantize(raw, out, cal_dir, per_channel=per_channel)
        rep = accuracy_delta(raw, out, cal_dir)
    except PrepError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    acc = {"samples": rep.samples, "top1_agreement_pct": rep.top1_agreement_pct,
           "mean_cosine": rep.mean_cosine, "max_abs_err": rep.max_abs_err,
           "verdict": rep.verdict, "guidance": rep.guidance}
    if fmt == "json":
        click.echo(json.dumps({"raw": str(raw), "quantized": str(out), "accuracy": acc}, indent=2))
        return
    click.echo(f"quantized: {out}")
    click.echo(f"accuracy: {rep.verdict}  top1={rep.top1_agreement_pct}%  "
               f"cosine={rep.mean_cosine}  max_abs_err={rep.max_abs_err}  (n={rep.samples})")
    if rep.guidance:
        click.echo(f"guidance: {rep.guidance}")


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
