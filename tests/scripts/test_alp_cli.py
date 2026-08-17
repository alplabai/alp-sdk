from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]


def test_alp_cli_help_lists_subcommands():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    for sub in ("init", "run", "emit", "validate",
                "doctor", "monitor", "explain", "faultdecode",
                "new-som"):
        assert sub in result.output


def test_alp_cli_reports_version():
    from alp_cli import __version__

    result = CliRunner().invoke(cli, ["--version"])
    assert result.exit_code == 0
    assert __version__ in result.output


def test_validate_passes_on_good_fixture():
    good = REPO / "tests" / "fixtures" / "board_yaml_good" / "minimal.yaml"
    result = CliRunner().invoke(cli, ["validate", str(good)])
    assert result.exit_code == 0


def test_validate_fails_on_bad_fixture_and_prints_code():
    bad = REPO / "tests" / "fixtures" / "board_yaml_bad" / "ALP-B001-missing-required.yaml"
    result = CliRunner().invoke(cli, ["validate", str(bad)])
    assert result.exit_code != 0
    assert "ALP-B001" in result.output


def test_init_non_interactive_scaffolds_project(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(
        cli,
        ["init", "my-app", "--som", "E1M-AEN801", "--preset", "e1m-evk",
         "--peripherals", "uart,gpio"],
    )
    assert result.exit_code == 0, result.output
    proj = tmp_path / "my-app"
    assert (proj / "board.yaml").is_file()
    assert (proj / "src" / "main.c").is_file()
    assert (proj / "CMakeLists.txt").is_file()
    board_yaml = (proj / "board.yaml").read_text(encoding="utf-8")
    assert "E1M-AEN801" in board_yaml
    assert "e1m-evk" in board_yaml


def test_init_refuses_existing_directory(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "already-there").mkdir()
    result = CliRunner().invoke(
        cli,
        ["init", "already-there", "--som", "E1M-AEN801", "--preset", "e1m-evk"],
    )
    assert result.exit_code != 0
    assert "already exists" in result.output


def test_run_reports_missing_board_yaml(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_run_finds_board_yaml_from_subdirectory(tmp_path: Path, monkeypatch, mocker):
    monkeypatch.chdir(tmp_path)
    proj = tmp_path / "proj"
    (proj / "src").mkdir(parents=True)
    (proj / "board.yaml").write_text("som:\n  sku: E1M-AEN801\npreset: e1m-evk\n")
    subdir = proj / "src"
    monkeypatch.chdir(subdir)
    # Mock the actual build/exec so the test doesn't shell out.
    called = {}

    def _stub(project_dir):
        called["dir"] = project_dir
        return 0

    mocker.patch("alp_cli.run._build_and_exec_native_sim", side_effect=_stub)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code == 0
    assert called["dir"] == proj


#: Verb names that exist in BOTH `alp_cli` and `tan` while meaning something
#: DIFFERENT (alp-sdk#1193, measured 2026-08-04 against tan 0.5.0):
#:
#:   generate  alp_cli: `generate TEMPLATE_ID DEST`, materialise a catalog
#:             template into a directory. tan: emit board-derived artefacts.
#:             Same name, opposite job -- the worst of the set.
#:   init      alp_cli: positional `NAME`. tan: options only, template/example
#:             driven, and the shipped onramp.
#:   doctor    alp_cli: `--json --strict`. tan: `--format json`, no `--strict`,
#:             and plain/`--build` are the same check set.
#:   explain   alp_cli: `explain CODE`, a diagnostic-code lookup. tan:
#:             `explain [TEMPLATE]`, describes generation targets.
#:   run       alp_cli: one direct `west build`. tan: build-then-run.
_COLLIDING_WITH_TAN = ("generate", "init", "doctor", "explain", "run")


def test_no_alp_console_script_while_the_tan_collisions_stand():
    """`pyproject.toml` must not grow an `alp` entry point (alp-sdk#1193).

    `alp_cli` is tan's Python BACKEND, invoked as `python -m alp_cli <sub>`
    (ADR 0020 §124-126). Installing it as a user-facing `alp` binary would put
    the verbs below on a customer's PATH under names that already mean
    something else in `tan` -- so `alp generate` and `tan generate` would be
    two different programs a user is expected to keep straight.

    This is a GUARD, not a design: the fix is to rename the colliding verbs
    and add per-verb parity tests first (alp-sdk#1193's remaining boxes).
    Until that lands, adding the entry point is the mistake this catches.
    """
    import tomllib

    with (REPO / "pyproject.toml").open("rb") as fh:
        scripts = tomllib.load(fh).get("project", {}).get("scripts", {})

    assert "alp" not in scripts, (
        "pyproject.toml grew an `alp` console-script while alp_cli still "
        f"shares {list(_COLLIDING_WITH_TAN)} with tan under incompatible "
        "contracts (alp-sdk#1193). Rename those verbs and add per-verb parity "
        "tests before exposing this package as a user-facing command."
    )


def test_the_colliding_verbs_still_exist_so_the_guard_is_not_vacuous():
    """The guard above is only meaningful while the collisions are real.

    If someone renames the colliding verbs -- the actual fix -- this fails and
    forces `_COLLIDING_WITH_TAN` and the guard's premise to be revisited,
    rather than leaving a test that passes because it no longer checks
    anything. A guard whose reason has quietly disappeared is the failure mode
    this pairs against.
    """
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    still_colliding = [v for v in _COLLIDING_WITH_TAN if v in result.output]
    assert still_colliding == list(_COLLIDING_WITH_TAN), (
        f"alp_cli no longer registers {sorted(set(_COLLIDING_WITH_TAN) - set(still_colliding))}. "
        "If the alp-sdk#1193 renames landed, update _COLLIDING_WITH_TAN and "
        "re-evaluate whether the `alp` entry-point guard above is still needed."
    )
