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

libraries:
  - name: mbedtls
    cores: [a55_cluster]
  - name: nlohmann-json
    cores: [a55_cluster]
  - name: cmsis-dsp
    cores: [m33_sm]

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
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


# The same multicore examples `scripts/check_emit_snapshots.py` pins a
# byte-for-byte golden for (ADR 0014) -- validating their real emitted plans
# here is the schema-side half of that same emitter <-> contract lockstep.
#
# rpmsg-imx93 excluded (#1025): E1M-NX9101's only hw_rev (imx93 r1) is
# `status: tbd` -- refused outright by the hw_rev-buildable gate, so it
# can no longer be emitted at all. Re-add
# "examples/multicore/rpmsg-imx93/board.yaml" (and its
# check_emit_snapshots.py CASES entries) once
# metadata/e1m_modules/imx93/hw-revisions.yaml:r1 carries a buildable status.
_PINNED_SNAPSHOT_BOARDS = [
    "examples/multicore/rpmsg-aen/board.yaml",
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
    at their defaults) exercises: the `baremetal` backend enum value, its
    EMPTY `configArtefacts` on a project with NO `preset:` (this board
    resolves no board name and E1M-AEN801 declares no restricted
    capabilities, so the slice has no `ALP_BOARD_<SLUG>`/`ALP_SOM_<SKU>`
    compile guard to carry and `alp-baremetal.cmake` is not emitted at
    all -- absence-emits-nothing; the preset-bearing case is covered in
    test_orchestrate_baremetal_slice.py), the baremetal `command` shape
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
    assert baremetal["configArtefacts"] == []
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
    case: `toolchain.targetTriple`/`toolchain.compiler` are the real
    Zephyr SDK arm-zephyr-eabi triple (SoM preset `topology.m55_hp.
    toolchain`), `artifacts.elf`/`.map`/`.bin`/`.compileCommands`
    follow Zephyr's own CMake output layout, and `debug.probe` is null
    for a Zephyr slice -- `system-manifest.yaml`'s `flash_method` no
    longer forces a runner (not every in-tree board registers
    `openocd`), so the resolved runner defers to the board.cmake
    default and `probe` stays null unless a runner is explicitly set."""
    board_yaml = REPO / "examples/multicore/rpmsg-aen/board.yaml"
    project = load_board_yaml(board_yaml)
    plan = json.loads(emit_build_plan(
        project, board_yaml=board_yaml, build_root=Path("build")))

    by_id = {s["coreId"]: s for s in plan["slices"]}
    m55_hp = by_id["m55_hp"]
    assert m55_hp["toolchain"] == {
        "targetTriple": "arm-zephyr-eabi",
        "compiler":     "arm-zephyr-eabi-gcc",
        "sysroot":      None,
        "id":           "arm-zephyr-eabi",
    }
    # Every path carries the `build/` level west actually writes (issue
    # #1360): the slice's `command` runs with cwd=`build/m55_hp-zephyr`
    # and no `-d`, so west's tree is `build/m55_hp-zephyr/build/`. The
    # old spelling (no `build/`) named files west never creates, and
    # every consumer had to add the level back by hand.
    assert m55_hp["artifacts"] == {
        "elf":             "build/m55_hp-zephyr/build/zephyr/zephyr.elf",
        "map":             "build/m55_hp-zephyr/build/zephyr/zephyr.map",
        "bin":             "build/m55_hp-zephyr/build/zephyr/zephyr.bin",
        "sizeReport":      "build/m55_hp-zephyr/build/zephyr/zephyr.stat",
        "symbols":         "build/m55_hp-zephyr/build/zephyr/zephyr.symbols",
        "compileCommands": "build/m55_hp-zephyr/build/compile_commands.json",
        # `outputDir` (alplabai/tan-cli#550) stays null for zephyr: the
        # six named paths above already index Zephyr's own output tree.
        "outputDir":       None,
    }
    assert m55_hp["debug"] == {"console": "uart", "probe": None}

    # The A-class Yocto slice: no single predictable ELF/compileCommands
    # output under buildDir (real output lives in the Yocto build tree's
    # own deploy dir) -- artifacts stay honestly null; toolchain.id is
    # still the real SoM preset toolchain tag (`poky-glibc`); debug.probe
    # is null (a Yocto image-flash recipe doesn't name a debug probe).
    a32 = by_id["a32_cluster"]
    assert a32["toolchain"]["id"] == "poky-glibc"
    assert a32["toolchain"]["targetTriple"] is None
    assert all(v is None for v in a32["artifacts"].values())
    assert a32["debug"] == {"console": "linux", "probe": None}

    for board_rel in _PINNED_SNAPSHOT_BOARDS:
        proj = load_board_yaml(REPO / board_rel)
        pl = json.loads(emit_build_plan(
            proj, board_yaml=REPO / board_rel, build_root=Path("build")))
        for sl in pl["slices"]:
            assert set(sl["toolchain"]) == {
                "targetTriple", "compiler", "sysroot", "id"}
            assert set(sl["artifacts"]) == {
                "elf", "map", "bin", "sizeReport", "symbols",
                "compileCommands", "outputDir"}
            assert set(sl["debug"]) == {"console", "probe"}


def test_baremetal_slice_toolchain_artifacts_debug_are_null(tmp_path: Path):
    """A `baremetal` slice's `debug` fields are all null, its
    NAMED artifacts (`elf`/`map`/`bin`/`sizeReport`/`symbols`) are all
    null, and `toolchain.targetTriple`/`.compiler` stay null too -- there
    is no SDK-wide vendor bare-toolchain / executable-name / debug-probe
    convention this emitter can predict without guessing (the app's own
    CMakeLists.txt picks its own executable name and cross toolchain
    file, and `arm-zephyr-eabi-gcc` is never actually invoked by the
    baremetal `cmake -S/-B` command). `toolchain.id` is the one
    exception: it passes through the SoM preset's `topology.m55_hp.
    toolchain` (`arm-zephyr-eabi`, set for that core's *default* zephyr
    role) verbatim regardless of this project's `os: baremetal`
    override -- an honest passthrough of the real resolved `Slice.
    toolchain` fact, not a fabricated value, even though it's a
    leftover from the core's un-overridden default.

    `outputDir` is the one exception, and it is NOT a guess: the slice's
    own configure carries
    `-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$<1:<buildDir>/output>`, so CMake
    is made to put every `add_executable()` target exactly there, on
    single- and multi-config generators alike (alplabai/tan-cli#550 --
    an all-null block left a slice that produced NO binary
    indistinguishable from one that built fine).

    `compileCommands` stays null even though the configure passes
    `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`: CMake implements that variable
    only for the Makefile and Ninja generator families and ignores it on
    every other generator, and this planner does not choose the
    generator."""
    path = _write_board(tmp_path, AEN801_BAREMETAL_AND_STOCK_IMAGE)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    by_id = {s["coreId"]: s for s in plan["slices"]}
    baremetal = by_id["m55_hp"]
    assert baremetal["toolchain"] == {
        "targetTriple": None, "compiler": None, "sysroot": None,
        "id": "arm-zephyr-eabi",
    }
    assert baremetal["artifacts"] == {
        "elf":             None,
        "map":             None,
        "bin":             None,
        "sizeReport":      None,
        "symbols":         None,
        "compileCommands": None,
        "outputDir":       "build/m55_hp-baremetal/output",
    }
    assert baremetal["debug"] == {"console": None, "probe": None}


def test_missing_required_field_rejected():
    """A plan missing a required field (here, a slice's `env`) fails
    validation -- the schema actually enforces its `required` arrays,
    it isn't just documentation."""
    p = _plan_with_tool("west")
    del p["slices"][0]["env"]
    validator = jsonschema.Draft202012Validator(_schema())
    errors = list(validator.iter_errors(p))
    assert errors, "missing required 'env' should have been rejected"


def test_execution_policy_absent_at_top_level_still_validates():
    """Strict-producer / tolerant-consumer (#855, reverting #847's breaking
    shape change): `executionPolicy` is no longer in the top-level
    `required` array, so a plan that omits it entirely -- e.g. every
    historical/v0.11.1 plan predating #847 -- still validates under
    schemaVersion 1 rather than being rejected. The real emitter keeps
    emitting it unconditionally on every plan regardless (see
    test_emit_build_plan_publishes_execution_policy in
    test_orchestrate_buildplan.py) -- this only relaxes what the SCHEMA
    accepts, not what the SDK actually produces."""
    ok = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        # "executionPolicy" deliberately omitted -- optional per the schema.
        "slices": [{
            "coreId": "m33_sm",
            "backend": "zephyr",
            "buildDir": "build/m33_sm-zephyr",
            "appDir": None,
            "configArtefacts": [],
            "toolchain": {
                "targetTriple": "arm-zephyr-eabi",
                "compiler": "arm-zephyr-eabi-gcc",
                "sysroot": None,
                "id": "arm-zephyr-eabi",
            },
            "artifacts": {
                "elf": None, "map": None, "bin": None,
                "sizeReport": None, "symbols": None,
                "compileCommands": None,
            },
            "debug": {"console": "uart", "probe": None},
            "command": None,
            "env": {"ALP_SDK_ROOT": "/repo"},
            "envAppendPath": {},
        }],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    errors = list(validator.iter_errors(ok))
    assert errors == [], "\n".join(str(e) for e in errors)


def test_execution_policy_still_validated_when_present():
    """`executionPolicy` stays a KNOWN, VALIDATED key -- optional, not
    schema-less. A plan that includes it with a malformed shape (missing
    `nullCommand`) is still rejected, same as before #855; only the
    top-level `required` entry was dropped."""
    bad = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "executionPolicy": {
            "unknownBackend": "fail",
            "missingTool": "skip",
            # "nullCommand" deliberately omitted -- required by the
            # executionPolicy sub-schema whenever the key is present.
        },
        "slices": [],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(bad)) != []


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


def _plan_with_tool(tool: str) -> dict:
    """A minimal otherwise-valid plan whose single slice's `command.tool`
    is the given string -- used to probe the `command.tool` identity
    convention (issue #1286) in isolation from every other field. Also
    reused as a minimal valid-plan base by tests that need one (e.g.
    test_missing_required_field_rejected)."""
    return {
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
                "targetTriple": "arm-zephyr-eabi",
                "compiler": "arm-zephyr-eabi-gcc",
                "sysroot": None,
                "id": "arm-zephyr-eabi",
            },
            "artifacts": {
                "elf": None, "map": None, "bin": None,
                "sizeReport": None, "symbols": None,
                "compileCommands": None,
            },
            "debug": {"console": "uart", "probe": None},
            "command": {"tool": tool, "args": [], "cwd": "build/m33_sm-zephyr"},
            "env": {"ALP_SDK_ROOT": "${SDK_ROOT}"},
            "envAppendPath": {},
        }],
        "sharedArtefacts": [],
        "warnings": [],
    }


def test_command_tool_schema_stays_tolerant_of_paths():
    """#847 precedent (also applied to `executionPolicy` above): the
    SHARED schema must not tighten a field's accepted shape at unchanged
    `schemaVersion: const 1`, or a consumer already holding a valid plan
    (e.g. one carrying a real path -- an IDE resolving `command.tool`
    itself before #1286's convention existed) gets rejected with no
    version signal. Every one of these was a valid `command.tool` before
    the #1286 change and must stay valid against the schema after it;
    the identity convention is enforced elsewhere (see the
    test_command_tool_*_gate tests in test_check_build_plan.py), never
    here."""
    validator = jsonschema.Draft202012Validator(_schema())
    for tool in (
        "west", "bitbake", "cmake",
        "/usr/bin/west", r"C:\x\west.exe", "${WEST}", "./west",
        ".venv/bin/west", "C:/tools/west.exe", "~/bin/west",
    ):
        errors = list(validator.iter_errors(_plan_with_tool(tool)))
        assert errors == [], f"{tool!r}: {[str(e) for e in errors]}"


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
