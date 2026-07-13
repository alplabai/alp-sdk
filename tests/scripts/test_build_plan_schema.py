# SPDX-License-Identifier: Apache-2.0
"""Tests for the build-plan v1 JSON Schema + its lockstep with
`scripts/alp_orchestrate/buildplan.py::emit_build_plan` (the `alp` CLI /
alp-sdk-vscode 'Wave C' consumer contract, see #610).

These pin: the schema itself is valid Draft 2020-12; the emitter's real
output for a representative multi-core project validates clean; the real
build-plan fixtures `scripts/check_emit_snapshots.py` pins for the four
multicore examples all conform; the currently-less-common emitter paths
(baremetal backend, sysbuild/TF-M conditional shared artefacts, the
`yocto-recipe-missing` warning, `appDir: null`) each validate too; and an
obviously-broken plan (missing a required field, an unknown top-level
key) is rejected -- so schema drift from the emitter is caught here
rather than downstream in the CLI.
"""
from __future__ import annotations

import json
import sys
import textwrap
from pathlib import Path

import jsonschema
import pytest

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"

sys.path.insert(0, str(REPO / "scripts"))
from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402

V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def test_schema_is_valid_draft202012():
    jsonschema.Draft202012Validator.check_schema(_schema())


def test_real_build_plan_conforms(tmp_path: Path):
    """The emitter's real output for a representative multi-core (Yocto
    + Zephyr) project validates against the schema with zero errors --
    the emitter <-> contract lockstep this schema exists to pin."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    errors = list(validator.iter_errors(plan))
    assert errors == [], "\n".join(str(e) for e in errors)

    # Sanity: this fixture actually exercises both a yocto and a zephyr
    # slice, plus at least one shared artefact -- so a passing validation
    # here is meaningful coverage, not a degenerate empty-plan pass.
    backends = {s["backend"] for s in plan["slices"]}
    assert backends == {"yocto", "zephyr"}
    assert plan["sharedArtefacts"]


# The same four multicore examples `scripts/check_emit_snapshots.py` pins a
# byte-for-byte golden for (ADR 0014) -- validating their real emitted plans
# here is the schema-side half of that same emitter <-> contract lockstep.
_PINNED_SNAPSHOT_BOARDS = [
    "examples/multicore/rpmsg-aen/board.yaml",
    "examples/multicore/rpmsg-imx93/board.yaml",
    "examples/multicore/heterogeneous-offload/board.yaml",
    "examples/multicore/rpmsg-v2n/board.yaml",
]


@pytest.mark.parametrize("board_rel", _PINNED_SNAPSHOT_BOARDS)
def test_pinned_emit_snapshot_boards_conform(board_rel: str):
    """The real board.yaml fixtures check_emit_snapshots.py pins a
    byte-for-byte golden for all emit a schema-conformant build plan."""
    board_yaml = REPO / board_rel
    project = load_board_yaml(board_yaml)
    plan = json.loads(emit_build_plan(
        project, board_yaml=board_yaml, build_root=Path("build")))
    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    errors = list(validator.iter_errors(plan))
    assert errors == [], "\n".join(str(e) for e in errors)


AEN801_BAREMETAL_AND_STOCK_IMAGE = """
som:
  sku: E1M-AEN801

cores:
  m55_hp:
    os: baremetal
    app: ./src
"""


def test_baremetal_slice_and_stock_image_appdir_null_conform(tmp_path: Path):
    """`os: baremetal` on m55_hp (with the SoM preset's other cores left
    at their defaults) exercises: the `baremetal` backend enum value,
    its `cmake-args.txt` configArtefact, the baremetal `command` shape
    (`tool: cmake`, `-S`/`-B` args), AND the A-class core's stock-image
    Yocto slice, which reports `appDir: null` (issue #597 -- there is no
    app source dir to report for the `alp-image-edge` token)."""
    path = _write_board(tmp_path, AEN801_BAREMETAL_AND_STOCK_IMAGE)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    assert list(validator.iter_errors(plan)) == []

    by_id = {s["coreId"]: s for s in plan["slices"]}
    baremetal = by_id["m55_hp"]
    assert baremetal["backend"] == "baremetal"
    assert baremetal["configArtefacts"][0]["path"].endswith("cmake-args.txt")
    assert baremetal["command"]["tool"] == "cmake"
    assert "-S" in baremetal["command"]["args"]
    assert "-B" in baremetal["command"]["args"]

    stock_image = by_id["a32_cluster"]
    assert stock_image["backend"] == "yocto"
    assert stock_image["appDir"] is None


AEN801_YOCTO_APP_NO_RECIPE = """
som:
  sku: E1M-AEN801

cores:
  a32_cluster:
    os: yocto
    app: ./linux
"""


def test_yocto_recipe_missing_warning_conforms(tmp_path: Path):
    """An app-only Yocto slice with no `recipe:` (issue #597) is carried
    with `command: null` plus a `yocto-recipe-missing` warning -- and the
    resulting plan still validates against the schema."""
    path = _write_board(tmp_path, AEN801_YOCTO_APP_NO_RECIPE)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    codes = [w["code"] for w in plan["warnings"]]
    assert "yocto-recipe-missing" in codes
    slice_ = next(s for s in plan["slices"] if s["coreId"] == "a32_cluster")
    assert slice_["command"] is None

    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    assert list(validator.iter_errors(plan)) == []


AEN301_MCUBOOT_AND_TFM = """
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: zephyr
    app: ./m55_he

boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/dev_ec.pem

storage:
  - name: psa_its
    size_kib: 64
    fs: raw
    flash_device: mram_main
  - name: psa_ps
    size_kib: 64
    fs: raw
    flash_device: mram_main

security:
  psa:
    persistent_slots: 32
    its_storage: psa_its
    ps_storage: psa_ps
    tfm: true
    attestation_root: optiga_trust_m
"""


def test_sysbuild_and_tfm_conditional_shared_artefacts_conform(tmp_path: Path):
    """`boot:` (-> build/alp_sysbuild.conf) and `security.psa.tfm: true`
    (-> build/sysbuild/tfm/tfm.conf) are both conditional sharedArtefacts
    (absence-emits-nothing); combined on one project they both appear,
    and the plan still validates against the schema."""
    path = _write_board(tmp_path, AEN301_MCUBOOT_AND_TFM)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    shared_paths = {a["path"] for a in plan["sharedArtefacts"]}
    assert any(p.endswith("alp_sysbuild.conf") for p in shared_paths)
    assert any(p.endswith("sysbuild/tfm/tfm.conf") for p in shared_paths)

    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    assert list(validator.iter_errors(plan)) == []


V2N_OFF_AND_COMMANDLESS = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: 'off'
  m33_sm:
    os: zephyr
"""


def test_commandless_slice_and_warning_conform(tmp_path: Path):
    """A slice the emitter cannot build yet (`command: null` + a
    `no-command` warning) is still a schema-valid plan -- the
    never-dropped-just-warned contract. `off` cores never enter the
    plan at all, and the schema's `backend` enum must not include
    `off` as a result."""
    path = _write_board(tmp_path, V2N_OFF_AND_COMMANDLESS)
    project = load_board_yaml(path)
    # Force the no-command path: no resolved board target, but a real
    # app dir so this isolates the board-missing case (mirrors
    # test_emit_build_plan_off_core_excluded_commandless_warns).
    project.cores["m33_sm"].board = None
    project.cores["m33_sm"].app = "./m33"
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    assert [s["coreId"] for s in plan["slices"]] == ["m33_sm"]
    assert plan["slices"][0]["command"] is None
    codes = [w["code"] for w in plan["warnings"]]
    assert "no-command" in codes

    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(plan)) == []


def test_pinned_snapshot_slices_carry_toolchain_artifacts_debug():
    """#610 §4 per-slice tooling index: every slice in the four pinned
    multicore examples carries the new `toolchain`/`artifacts`/`debug`
    objects (schema `required`, so a validating plan already proves
    their presence -- this pins concrete derived *values*, not just
    shape). The AEN example's `m55_hp` Zephyr slice is the ground-truth
    case: `toolchain.target_triple`/`toolchain.compiler` are the real
    Zephyr SDK arm-zephyr-eabi triple (SoM preset `topology.m55_hp.
    toolchain`), `artifacts.elf`/`.map`/`.bin`/`.compile_commands`
    follow Zephyr's own CMake output layout, and `debug.probe` is the
    same `openocd` runner `system-manifest.yaml`'s `flash_method`
    resolves to for a Zephyr slice."""
    board_yaml = REPO / "examples/multicore/rpmsg-aen/board.yaml"
    project = load_board_yaml(board_yaml)
    plan = json.loads(emit_build_plan(
        project, board_yaml=board_yaml, build_root=Path("build")))

    by_id = {s["coreId"]: s for s in plan["slices"]}
    m55_hp = by_id["m55_hp"]
    assert m55_hp["toolchain"] == {
        "target_triple": "arm-zephyr-eabi",
        "compiler":      "arm-zephyr-eabi-gcc",
        "sysroot":       None,
        "id":            "arm-zephyr-eabi",
    }
    assert m55_hp["artifacts"] == {
        "elf":              "build/m55_hp-zephyr/zephyr/zephyr.elf",
        "map":              "build/m55_hp-zephyr/zephyr/zephyr.map",
        "bin":              "build/m55_hp-zephyr/zephyr/zephyr.bin",
        "size_report":      "build/m55_hp-zephyr/zephyr/zephyr.stat",
        "symbols":          "build/m55_hp-zephyr/zephyr/zephyr.symbols",
        "compile_commands": "build/m55_hp-zephyr/compile_commands.json",
    }
    assert m55_hp["debug"] == {"console": "uart", "probe": "openocd"}

    # The A-class Yocto slice: no single predictable ELF/compile_commands
    # output under buildDir (real output lives in the Yocto build tree's
    # own deploy dir) -- artifacts stay honestly null; toolchain.id is
    # still the real SoM preset toolchain tag (`poky-glibc`); debug.probe
    # is null (a Yocto image-flash recipe doesn't name a debug probe).
    a32 = by_id["a32_cluster"]
    assert a32["toolchain"]["id"] == "poky-glibc"
    assert a32["toolchain"]["target_triple"] is None
    assert all(v is None for v in a32["artifacts"].values())
    assert a32["debug"] == {"console": "linux", "probe": None}

    for board_rel in _PINNED_SNAPSHOT_BOARDS:
        proj = load_board_yaml(REPO / board_rel)
        pl = json.loads(emit_build_plan(
            proj, board_yaml=REPO / board_rel, build_root=Path("build")))
        for sl in pl["slices"]:
            assert set(sl["toolchain"]) == {
                "target_triple", "compiler", "sysroot", "id"}
            assert set(sl["artifacts"]) == {
                "elf", "map", "bin", "size_report", "symbols",
                "compile_commands"}
            assert set(sl["debug"]) == {"console", "probe"}


def test_baremetal_slice_toolchain_artifacts_debug_are_null(tmp_path: Path):
    """A `baremetal` slice's `artifacts` + `debug` fields are all null,
    and `toolchain.target_triple`/`.compiler` stay null too -- there is
    no SDK-wide vendor bare-toolchain / executable-name / debug-probe
    convention this emitter can predict without guessing (the app's own
    CMakeLists.txt picks its own executable name and cross toolchain
    file, and `arm-zephyr-eabi-gcc` is never actually invoked by the
    baremetal `cmake -S/-B` command). `toolchain.id` is the one
    exception: it passes through the SoM preset's `topology.m55_hp.
    toolchain` (`arm-zephyr-eabi`, set for that core's *default* zephyr
    role) verbatim regardless of this project's `os: baremetal`
    override -- an honest passthrough of the real resolved `Slice.
    toolchain` fact, not a fabricated value, even though it's a
    leftover from the core's un-overridden default."""
    path = _write_board(tmp_path, AEN801_BAREMETAL_AND_STOCK_IMAGE)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    by_id = {s["coreId"]: s for s in plan["slices"]}
    baremetal = by_id["m55_hp"]
    assert baremetal["toolchain"] == {
        "target_triple": None, "compiler": None, "sysroot": None,
        "id": "arm-zephyr-eabi",
    }
    assert all(v is None for v in baremetal["artifacts"].values())
    assert baremetal["debug"] == {"console": None, "probe": None}


def test_missing_required_field_rejected():
    """A plan missing a required field (here, a slice's `env`) fails
    validation -- the schema actually enforces its `required` arrays,
    it isn't just documentation."""
    bad = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [{
            "coreId": "m33_sm",
            "backend": "zephyr",
            "buildDir": "build/m33_sm-zephyr",
            "appDir": None,
            "configArtefacts": [],
            "toolchain": {
                "target_triple": "arm-zephyr-eabi",
                "compiler": "arm-zephyr-eabi-gcc",
                "sysroot": None,
                "id": "arm-zephyr-eabi",
            },
            "artifacts": {
                "elf": None, "map": None, "bin": None,
                "size_report": None, "symbols": None,
                "compile_commands": None,
            },
            "debug": {"console": "uart", "probe": "openocd"},
            "command": None,
            # "env" deliberately omitted -- required by the schema.
        }],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    errors = list(validator.iter_errors(bad))
    assert errors, "missing required 'env' should have been rejected"


def test_unknown_top_level_key_rejected():
    """`additionalProperties: false` at the top level catches drift/typos
    the way `check_system_manifest.py`'s contract does for the sibling
    system-manifest schema."""
    bad = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [],
        "sharedArtefacts": [],
        "warnings": [],
        "bogusKey": 1,
    }
    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(bad)) != []


def test_wrong_schema_version_rejected():
    """`schemaVersion` is a locked `const` -- any other value (e.g. a
    future breaking bump the consumer hasn't been told about yet) must
    fail rather than silently validate."""
    bad = {
        "schemaVersion": 2,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(bad)) != []
