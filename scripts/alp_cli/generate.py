"""`alp generate` -- materialise a project template into a fresh directory.

Thin CLI wrapper over scripts/alp_template.py's render()/preview -- the
deterministic engine that owns copying a template's `files.user_owned`
out of its canonical example (metadata/templates/catalog-v1.json),
parameter validation/substitution, and the temp-dir "validated before
publication" gate (`alp_template.validate()`, invoked directly, not
through this CLI -- it needs ZEPHYR_BASE + twister, so it stays a
library/test entry point rather than an interactive command).

Deliberately a separate verb from `alp init`: `init` scaffolds one fixed
copy of examples/peripheral-io/hello-world by string-patching in a
SoM/preset/peripheral list (an interactive SoM-bring-up wizard), while
`generate` materialises ANY catalogued template by id with a real
preview/force/param contract. The two commands take incompatible
arguments (`init NAME` creates NAME/ under cwd; `generate ID DEST`
targets an explicit destination) and serve different jobs, so this is
a new command rather than an awkward extension of `init`.
"""

from __future__ import annotations

import sys
from pathlib import Path

import click

from alp_cli._workspace import sdk_root


def _load_alp_template():
    root = sdk_root()
    if root is None:
        click.echo(
            "alp generate: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)
    # alp_template.py lives at <sdk>/scripts/alp_template.py -- not under
    # the alp_cli package (see pyproject.toml's packages.find), so make
    # sure <sdk>/scripts is importable before reaching for it. Mirrors
    # alp_cli/doctor.py's _library_check() sys.path handling for the same
    # top-level-scripts-module situation.
    scripts_dir = str(root / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    import alp_template

    return alp_template


def _parse_params(pairs: tuple[str, ...]) -> dict[str, str]:
    params: dict[str, str] = {}
    for raw in pairs:
        if "=" not in raw:
            click.echo(
                f"alp generate: --param expects name=value, got {raw!r}",
                err=True,
            )
            raise SystemExit(1)
        name, _, value = raw.partition("=")
        params[name] = value
    return params


@click.command(
    name="generate",
    help="Materialise a catalog template into a fresh directory.",
)
@click.argument("template_id")
@click.argument("dest", type=click.Path(path_type=Path))
@click.option(
    "--dry-run", is_flag=True,
    help="List what would be written; touch nothing.",
)
@click.option(
    "--force", is_flag=True, help="Allow overwriting a non-empty dest.",
)
@click.option(
    "--param", "params", multiple=True, metavar="name=value",
    help="Override a declared template parameter (repeatable).",
)
def generate_cmd(
    template_id: str,
    dest: Path,
    dry_run: bool,
    force: bool,
    params: tuple[str, ...],
) -> None:
    alp_template = _load_alp_template()
    resolved_params = _parse_params(params)
    try:
        result = alp_template.render(
            template_id, dest, resolved_params, dry_run=dry_run, force=force,
        )
    except alp_template.TemplateError as exc:
        click.echo(f"alp generate: {exc}", err=True)
        raise SystemExit(1)

    verb = "would write" if dry_run else "wrote"
    for rel in result.files:
        click.echo(f"{verb} {dest / rel}")
    for name, default, value in result.substitutions:
        click.echo(f"  substituting {name}: {default!r} -> {value!r}")
    if not dry_run:
        click.echo(f"Created {dest}/ from template '{template_id}'.")
        click.echo("Next: cd " + str(dest) + " && alp run")
