"""Tests for the ADR 0018 curated third-party library layer.

Covers the four surfaces the feature adds:

  * the ``metadata/schemas/library-v1.schema.json`` schema + the
    ``validate_metadata.py`` semantic pass (accept the real manifests;
    reject a bad tier / licence / capability key / filename mismatch);
  * the orchestrator emit (top-level ``libraries:`` -> Zephyr Kconfig /
    Yocto IMAGE_INSTALL; incompatible + unknown selections raise a clear
    ``OrchestratorError``; zero-diff when unused);
  * the reporting surface -- the tier / licence / fit facts any reporter
    renders, asserted on the layer itself.  The CLI that renders them is
    ``tan doctor`` (ADR 0020 end-state B), not this repo; see the section
    comment above those cases.
"""

import json
import sys
import textwrap
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import load_board_yaml  # noqa: E402
from alp_orchestrate.kconfig import _slice_alp_conf, _slice_local_conf  # noqa: E402
from alp_orchestrate.models import BoardProject, OrchestratorError, Slice  # noqa: E402
from alp_orchestrate import libraries as liblayer  # noqa: E402

LIBRARY_SCHEMA = REPO / "metadata" / "schemas" / "library-v1.schema.json"
LIBRARIES_DIR = REPO / "metadata" / "libraries"

EXPECTED_LIBS = {"lvgl", "cmsis-dsp", "cmsis-nn", "nanopb", "zcbor", "modbus"}


def _write_board(tmp: Path, body: str) -> Path:
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _validator() -> jsonschema.Draft202012Validator:
    schema = json.loads(LIBRARY_SCHEMA.read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def _valid_manifest() -> dict:
    """A minimal schema-valid manifest used as the base for reject cases."""
    return {
        "schema_version": 1,
        "name": "widget",
        "tier": "A",
        "version": "1.0.0",
        "license": "MIT",
        "integration": {"zephyr": {"kconfig": ["CONFIG_WIDGET=y"]}},
    }


# ---------------------------------------------------------------------
# Schema: the real manifests + the enumerated field set
# ---------------------------------------------------------------------

def test_all_expected_manifests_present() -> None:
    on_disk = {p.stem for p in LIBRARIES_DIR.glob("*.yaml")}
    assert EXPECTED_LIBS <= on_disk, f"missing manifests: {EXPECTED_LIBS - on_disk}"


@pytest.mark.parametrize("path", sorted(LIBRARIES_DIR.glob("*.yaml")),
                         ids=lambda p: p.stem)
def test_real_manifest_schema_valid(path: Path) -> None:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    errors = sorted(_validator().iter_errors(doc), key=lambda e: list(e.absolute_path))
    assert not errors, [e.message for e in errors]
    # name must match filename (the `libraries: [<name>]` token resolves by it).
    assert doc["name"] == path.stem


def test_schema_rejects_bad_tier() -> None:
    doc = _valid_manifest()
    doc["tier"] = "C"
    assert list(_validator().iter_errors(doc)), "tier C must be rejected"


def test_schema_rejects_bad_license() -> None:
    doc = _valid_manifest()
    doc["license"] = "GPL-3.0-only"
    assert list(_validator().iter_errors(doc)), "GPL licence must be rejected"


def test_schema_requires_an_integration_section() -> None:
    doc = _valid_manifest()
    doc["integration"] = {}
    assert list(_validator().iter_errors(doc)), "empty integration must be rejected"


def test_schema_rejects_unknown_top_level_key() -> None:
    doc = _valid_manifest()
    doc["fetch"] = "https://example.invalid/x.tar.gz"
    assert list(_validator().iter_errors(doc)), "additionalProperties must be false"


# ---------------------------------------------------------------------
# validate_metadata semantic pass
# ---------------------------------------------------------------------

def test_validate_metadata_semantics_accepts_real_manifests() -> None:
    import validate_metadata as vm
    failures = vm._check_library_semantics(sorted(LIBRARIES_DIR.glob("*.yaml")))
    assert failures == []


def test_validate_metadata_rejects_unknown_capability_key(tmp_path: Path) -> None:
    import validate_metadata as vm
    bad = tmp_path / "badcap.yaml"
    doc = _valid_manifest()
    doc["name"] = "badcap"
    doc["requires"] = {"capabilities": ["display"]}  # not a real SoC cap
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])
    assert failures, "unknown capability key must fail the semantic pass"
    assert "capabilities" in failures[0][1][0]


def test_validate_metadata_rejects_name_filename_mismatch(tmp_path: Path) -> None:
    import validate_metadata as vm
    bad = tmp_path / "onthedisk.yaml"
    doc = _valid_manifest()
    doc["name"] = "different"
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])
    assert failures and "must match the manifest filename" in failures[0][1][0]


def test_non_list_requires_capabilities_does_not_crash_the_gate(tmp_path: Path) -> None:
    """`requires.capabilities` is itself schema-typed as an array, but a
    malformed manifest can carry a non-list scalar there (e.g. the bare int
    `5`, which is truthy) -- iterating the unfiltered value used to raise
    `TypeError: 'int' object is not iterable`, aborting the whole gate
    mid-run instead of leaving the schema FAIL line (which already flags
    the type mismatch) to explain the real problem."""
    import validate_metadata as vm
    bad = tmp_path / "badcap2.yaml"
    doc = _valid_manifest()
    doc["name"] = "badcap2"
    doc["requires"] = {"capabilities": 5}
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])  # must not raise
    assert failures == []


def test_non_string_requires_capability_entry_does_not_crash_the_gate(tmp_path: Path) -> None:
    """`requires.capabilities[]` entries are schema-typed as strings, but a
    malformed manifest can carry a dict/list entry there -- the unfiltered
    `cap not in vocab` membership test used to raise `TypeError: unhashable
    type: 'dict'`."""
    import validate_metadata as vm
    bad = tmp_path / "badcap3.yaml"
    doc = _valid_manifest()
    doc["name"] = "badcap3"
    doc["requires"] = {"capabilities": [{"nested": "dict"}]}
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])  # must not raise
    assert failures  # non-string capability is reported as unknown


def test_capability_vocabulary_is_grounded() -> None:
    import validate_metadata as vm
    vocab = vm._capability_vocabulary()
    # A few keys that must exist per soc-spec-v1; and `display` must NOT
    # (there is no display capability -- the reason lvgl gates on RAM only).
    assert {"gpu2d", "dave2d", "cryptocell"} <= vocab
    assert "display" not in vocab


# ---------------------------------------------------------------------
# Emit: Zephyr Kconfig
# ---------------------------------------------------------------------

_V2N_LVGL = """
som:
  sku: E1M-V2N101
libraries: [lvgl]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_DSP = """
som:
  sku: E1M-V2N101
libraries: [cmsis-dsp]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_NOLIB = """
som:
  sku: E1M-V2N101
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_CMSIS_STREAM = """
som:
  sku: E1M-V2N101
libraries: [cmsis-stream]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_CMSIS_CV = """
som:
  sku: E1M-V2N101
libraries: [cmsis-cv]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_ARM_2D = """
som:
  sku: E1M-V2N101
libraries: [arm-2d]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_lvgl_zephyr_kconfig(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_LVGL))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_LVGL=y" in out
    assert "ADR 0018" in out
    assert "lvgl v9.5.0" in out  # version transcribed from the manifest


def test_emit_cmsis_dsp_zephyr_kconfig(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_DSP))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_CMSIS_DSP=y" in out
    assert "CONFIG_CMSIS_DSP_TRANSFORM=y" in out


def test_emit_cmsis_stream_zephyr_kconfig(tmp_path: Path) -> None:
    """cmsis-stream names a real upstream west module (`cmsisstream`, its own
    zephyr/module.yml at the pinned v3.2.0 tag) and its umbrella Kconfig."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_CMSIS_STREAM))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_CMSISSTREAM=y" in out
    assert "cmsis-stream v3.2.0" in out  # version transcribed from the manifest


def test_emit_cmsis_cv_module_only_no_kconfig(tmp_path: Path) -> None:
    """cmsis-cv has no upstream Zephyr module glue (no zephyr/module.yml, no
    Kconfig at the pinned SHA) -- `module: null`, so emit must not fabricate
    a CONFIG line."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_CMSIS_CV))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "# library: cmsis-cv v25c6c111ee04dcfb0ae9093fd6dee4586872982c" in out
    # Nothing invented: the library layer emits the tag line and nothing else.
    assert liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"], liblayer.METADATA_ROOT) == [
        "# library: cmsis-cv v25c6c111ee04dcfb0ae9093fd6dee4586872982c"
    ]


def test_emit_arm_2d_module_only_no_kconfig(tmp_path: Path) -> None:
    """arm-2d has a mature tagged release but no upstream Zephyr module glue
    (no zephyr/module.yml, no Kconfig) -- `module: null`, so emit must not
    fabricate a CONFIG line."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_ARM_2D))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "# library: arm-2d v1.2.6" in out
    # Nothing invented: the library layer emits the tag line and nothing else.
    assert liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"], liblayer.METADATA_ROOT) == [
        "# library: arm-2d v1.2.6"
    ]


def test_emit_zero_diff_without_libraries(tmp_path: Path) -> None:
    """A project that declares no `libraries:` must not gain the library block."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "ADR 0018" not in out
    assert "CONFIG_LVGL=y" not in out
    # Helper returns nothing for an unselected project (guards the guard).
    assert liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"], liblayer.METADATA_ROOT) == []


def test_emit_unknown_library_lists_available(tmp_path: Path) -> None:
    body = _V2N_NOLIB.replace("cores:", "libraries: [lvglx]\ncores:")
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"], liblayer.METADATA_ROOT)
    msg = str(exc.value)
    assert "unknown library `lvglx`" in msg
    # lists the available manifests so the typo is self-correcting
    assert "lvgl" in msg and "cmsis-dsp" in msg


def test_library_scoped_to_undeclared_core_is_rejected(tmp_path: Path) -> None:
    """A `libraries:` entry whose `cores:` names a core the topology doesn't
    declare is a hard error -- silently dropping it would emit nothing for a
    library the app author explicitly asked for."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: lvgl
        cores: [ghost_core]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
    """
    with pytest.raises(OrchestratorError) as exc:
        load_board_yaml(_write_board(tmp_path, body))
    msg = str(exc.value)
    assert "ghost_core" in msg
    assert "lvgl" in msg


# ---------------------------------------------------------------------
# Emit: compatibility errors name the failing constraint
# ---------------------------------------------------------------------

def test_requires_min_ram_names_constraint(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    manifest = {"requires": {"min_ram_kib": 10 ** 9}}
    with pytest.raises(OrchestratorError) as exc:
        liblayer._check_requires("hog", manifest, project, liblayer.METADATA_ROOT)
    assert "min_ram_kib" in str(exc.value)


def test_requires_capability_names_constraint(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    manifest = {"requires": {"capabilities": ["gpu2d"]}}  # V2N has no gpu2d cap
    with pytest.raises(OrchestratorError) as exc:
        liblayer._check_requires("needsgpu", manifest, project, liblayer.METADATA_ROOT)
    assert "gpu2d" in str(exc.value)


def test_incompatible_selection_not_wireable(tmp_path: Path) -> None:
    """cmsis-nn is Zephyr-only; a project whose only live core runs yocto
    cannot wire it -- resolve_selection must reject naming the mismatch."""
    body = """
    som:
      sku: E1M-V2N101
    libraries: [cmsis-nn]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.resolve_selection(project, liblayer.METADATA_ROOT)
    assert "cannot be wired" in str(exc.value)


# ---------------------------------------------------------------------
# Emit: Yocto IMAGE_INSTALL
# ---------------------------------------------------------------------

def test_emit_lvgl_yocto_image_install(tmp_path: Path) -> None:
    body = """
    som:
      sku: E1M-V2N101
    libraries: [lvgl]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert 'IMAGE_INSTALL:append = " lvgl"' in out


# ---------------------------------------------------------------------
# Reporting surface: tier + licence + fit, straight off the layer
#
# These three used to drive `alp_cli.doctor._check_libraries` -- the doctor
# LINE, not the layer.  That check moved to `tan doctor` (`tan.core.
# doctor_libraries`): under ADR 0020 end-state B `tan` is the whole user
# command surface, so alp-sdk does not keep a second CLI reporting on its own
# library layer.  What alp-sdk still owes any reporter is the DATA -- that
# `tier`/`license` are present and readable, that a selection resolves, and
# that an empty selection resolves to nothing -- so that is what these assert
# now, with no CLI in the path.
#
# Driving the doctor line was in fact WORSE coverage of the layer, not
# better: all three wrote the bare-string `libraries: [lvgl]` shape, which is
# the one shape no shipped example uses.  Every in-tree example writes
# `- {name: ..., cores: [...]}`, and against that shape the check under test
# raised `OrchestratorError` straight out of `_all_checks()` -- exit 1, whole
# command down -- because it labelled from the RAW document instead of the
# loader's normalised names.  Three green tests over a check that worked on
# no real project.  `test_library_report_covers_both_declaration_shapes`
# below is that missing case, asserted where it belongs.
# ---------------------------------------------------------------------

def test_library_manifests_carry_the_tier_and_licence_a_report_renders() -> None:
    """`lvgl (tier A, MIT)` -- the exact string a reporter builds -- is
    assembled from manifest fields, so those fields must be present and
    readable through the layer's own accessor rather than by reading YAML."""
    manifest = liblayer.load_manifest("lvgl")
    assert manifest["tier"] == "A"
    assert manifest["license"] == "MIT"


def test_a_selected_library_resolves_with_its_manifest(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_LVGL))
    resolved = liblayer.resolve_selection(project, REPO / "metadata")
    assert [name for name, _ in resolved] == ["lvgl"]
    assert resolved[0][1]["tier"] == "A"
    assert resolved[0][1]["license"] == "MIT"


def test_a_project_selecting_nothing_resolves_to_nothing(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    assert liblayer.resolve_selection(project, REPO / "metadata") == []


def test_library_report_covers_both_declaration_shapes(tmp_path: Path) -> None:
    """`scoped_names` is the only correct source of names for a reporter:
    board.yaml declares a library EITHER as a bare string (project-wide) OR as
    `{name, cores}` (core-scoped), and every shipped alp-sdk example uses the
    second.  A reporter that reads the raw `libraries:` list gets a dict where
    it expected a name."""
    body = _V2N_NOLIB.replace(
        "cores:", "libraries:\n  - name: lvgl\n    cores: [m33_sm]\ncores:"
    )
    project = load_board_yaml(_write_board(tmp_path, body))
    assert liblayer.scoped_names(project) == ["lvgl"]
    resolved = liblayer.resolve_selection(project, REPO / "metadata")
    assert [name for name, _ in resolved] == ["lvgl"]


# ---------------------------------------------------------------------
# ADR 0018 flagship: micro-ROS (M / Zephyr) + ROS 2 (A55 / Yocto)
#
# The heterogeneous-core proof (ADR 0010): one project, a micro-ROS node on
# the Cortex-M Zephyr peer and ROS 2 on the Cortex-A Yocto peer.  Grounding
# (2026-07-03): micro-ROS is NOT pinned in the Zephyr v4.4.0 west.yml, so its
# manifest documents the west prerequisite and declares an enable-by-presence
# module with NO fabricated Kconfig; ROS 2 IS grounded via meta-alp-sdk
# (rclcpp in alp-image-common.inc, meta-ros2-humble LAYERRECOMMENDS).
# ---------------------------------------------------------------------

FLAGSHIP_LIBS = {"micro-ros", "ros2"}


def test_flagship_manifests_present() -> None:
    on_disk = {p.stem for p in LIBRARIES_DIR.glob("*.yaml")}
    assert FLAGSHIP_LIBS <= on_disk, f"missing flagship manifests: {FLAGSHIP_LIBS - on_disk}"


def test_ros2_is_tier_b() -> None:
    """ROS 2 is a heavy Yocto layer, not an alp-sdk-CI-built library -> Tier B."""
    doc = yaml.safe_load((LIBRARIES_DIR / "ros2.yaml").read_text(encoding="utf-8"))
    assert doc["tier"] == "B"
    assert doc["license"] == "Apache-2.0"


def test_microros_is_apache() -> None:
    doc = yaml.safe_load((LIBRARIES_DIR / "micro-ros.yaml").read_text(encoding="utf-8"))
    assert doc["license"] == "Apache-2.0"
    zephyr = doc["integration"]["zephyr"]
    assert zephyr.get("module") == "micro_ros_zephyr_module"
    assert zephyr.get("kconfig") == ["CONFIG_MICROROS=y"]


# --- schema: the module-only Zephyr section the flagship needs -------

def test_schema_accepts_zephyr_module_only() -> None:
    """A Zephyr integration may name its west module without a Kconfig -- the
    honest shape for an enable-by-presence / not-yet-pinned module (micro-ROS).
    """
    doc = _valid_manifest()
    doc["integration"] = {"zephyr": {"module": "micro_ros_zephyr_module"}}
    assert not list(_validator().iter_errors(doc)), "module-only zephyr must validate"


def test_schema_rejects_empty_zephyr_section() -> None:
    """Relaxing `kconfig` must not let an empty zephyr section through: it still
    has to carry at least a module (minProperties: 1)."""
    doc = _valid_manifest()
    doc["integration"] = {"zephyr": {}}
    assert list(_validator().iter_errors(doc)), "empty zephyr section must be rejected"


def test_schema_rejects_floating_west_revision() -> None:
    """A manifest-provided west project pin must be reproducible."""
    doc = _valid_manifest()
    doc["integration"] = {
        "zephyr": {
            "module": "widget",
            "west": {
                "name": "widget",
                "url": "https://github.com/example/widget.git",
                "revision": "main",
                "path": "modules/lib/widget",
            },
        }
    }
    assert list(_validator().iter_errors(doc)), "floating west revision must fail"


# --- emit: micro-ROS on an M-core -----------------------------------

_V2N_MICROROS = """
som:
  sku: E1M-V2N101
libraries: [micro-ros]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_microros_module_and_kconfig(tmp_path: Path) -> None:
    """micro-ROS on the M33 emits the ADR 0018 selection tag naming the west
    module plus the real upstream master enable symbol."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_MICROROS))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "ADR 0018" in out
    assert "micro_ros_zephyr_module" in out           # module named in the tag
    assert "micro-ros vhumble" in out                 # version transcribed
    assert "CONFIG_MICROROS=y" in out                 # real module Kconfig


def test_microros_requires_zephyr_core(tmp_path: Path) -> None:
    """micro-ROS is the M/Zephyr client; selecting it on a project whose live
    cores run no Zephyr fails naming the os constraint."""
    body = """
    som:
      sku: E1M-V2N101
    libraries: [micro-ros]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.resolve_selection(project, liblayer.METADATA_ROOT)
    assert "zephyr" in str(exc.value)


# --- emit: ROS 2 on an A55 Yocto core -------------------------------

_V2N_ROS2_YOCTO = """
som:
  sku: E1M-V2N101
libraries: [ros2]
cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
"""


def test_emit_ros2_yocto_image_install(tmp_path: Path) -> None:
    """ROS 2 on the A55 Yocto slice appends the grounded rclcpp package
    (transcribed from meta-alp-sdk alp-image-common.inc)."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_ROS2_YOCTO))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert 'IMAGE_INSTALL:append = " rclcpp"' in out


def test_ros2_on_non_yocto_target_errors(tmp_path: Path) -> None:
    """ROS 2 requires os: [yocto] + core_class a; a project whose only live
    core is the M33 running Zephyr fails naming the os constraint."""
    body = """
    som:
      sku: E1M-V2N101
    libraries: [ros2]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.resolve_selection(project, liblayer.METADATA_ROOT)
    msg = str(exc.value)
    assert "ros2" in msg and "yocto" in msg


# ---------------------------------------------------------------------
# ADR 0018 cloud / connectivity Tier-B group
#
# Two upstream Zephyr subsystems (lwm2m, coap -- real CONFIG symbols grounded
# in the pinned v4.4.0 tree) and two pinned generic C SDKs (aws-iot, azure-iot
# -- west project pins, enable-by-presence module named, no fabricated Kconfig).
# Grounding (2026-07-05):
#   * CONFIG_LWM2M  $ZEPHYR_BASE/subsys/net/lib/lwm2m/Kconfig `menuconfig LWM2M`
#   * CONFIG_COAP   $ZEPHYR_BASE/subsys/net/lib/coap/Kconfig  `config COAP`
#   * aws-iot / azure-iot: no upstream Zephyr west module in `west list`; exact
#     release pins live in integration.zephyr.west, no CONFIG is invented.
# ---------------------------------------------------------------------

CLOUD_LIBS = {"lwm2m", "coap", "aws-iot", "azure-iot"}


def test_cloud_manifests_present() -> None:
    on_disk = {p.stem for p in LIBRARIES_DIR.glob("*.yaml")}
    assert CLOUD_LIBS <= on_disk, f"missing cloud manifests: {CLOUD_LIBS - on_disk}"


def test_lwm2m_coap_are_upstream_apache() -> None:
    for lib in ("lwm2m", "coap"):
        doc = yaml.safe_load((LIBRARIES_DIR / f"{lib}.yaml").read_text(encoding="utf-8"))
        assert doc["tier"] == "B"
        assert doc["license"] == "Apache-2.0"


def test_aws_azure_are_module_only_prerequisites() -> None:
    """The cloud manifests name real upstream repos with exact west pins and NO
    fabricated Kconfig (generic C SDKs, enable-by-presence)."""
    aws = yaml.safe_load((LIBRARIES_DIR / "aws-iot.yaml").read_text(encoding="utf-8"))
    assert aws["license"] == "Apache-2.0"
    assert aws["version"] == "v3.1.5"
    zephyr = aws["integration"]["zephyr"]
    assert zephyr.get("module") == "aws-iot-device-sdk-embedded-C"
    assert zephyr["west"]["revision"] == "v3.1.5"
    assert zephyr["west"]["path"] == "modules/lib/aws-iot-device-sdk-embedded-C"
    assert "kconfig" not in zephyr, "no Kconfig may be invented without a real symbol"

    azure = yaml.safe_load((LIBRARIES_DIR / "azure-iot.yaml").read_text(encoding="utf-8"))
    assert azure["license"] == "MIT"
    assert azure["version"] == "1.5.0"
    zephyr = azure["integration"]["zephyr"]
    assert zephyr.get("module") == "azure-sdk-for-c"
    assert zephyr["west"]["revision"] == "1.5.0"
    assert zephyr["west"]["path"] == "modules/lib/azure-sdk-for-c"
    assert "kconfig" not in zephyr


# --- emit: an upstream cloud lib lands its real CONFIG on a Zephyr M core ---

_V2N_LWM2M = """
som:
  sku: E1M-V2N101
libraries: [lwm2m, coap]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_lwm2m_coap_zephyr_kconfig(tmp_path: Path) -> None:
    """lwm2m + coap on the M33 emit their grounded upstream enable symbols."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_LWM2M))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_LWM2M=y" in out
    assert "CONFIG_COAP=y" in out
    assert "ADR 0018" in out
    assert "lwm2m v4.4.1" in out  # version transcribed from the manifest


# --- emit: a prerequisite cloud lib emits the tag with NO fabricated CONFIG ---

_V2N_AWS = """
som:
  sku: E1M-V2N101
libraries: [aws-iot]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_aws_iot_module_only_no_kconfig(tmp_path: Path) -> None:
    """aws-iot on the M33 emits the ADR 0018 selection tag naming the upstream
    module and -- because the SDK has no confirmed enable symbol -- NO
    fabricated CONFIG line."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_AWS))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "ADR 0018" in out
    assert "aws-iot-device-sdk-embedded-C" in out   # module named in the tag
    assert "aws-iot v3.1.5" in out                   # version transcribed
    assert "CONFIG_AWS" not in out                   # nothing invented


# --- os constraint on an upstream cloud lib names the failing constraint ---

def test_lwm2m_on_non_zephyr_target_errors(tmp_path: Path) -> None:
    """lwm2m requires os: [zephyr]; a project whose only live core is the A55
    running Yocto fails naming the os constraint (upstream-lib variant of the
    incompatibility path)."""
    body = """
    som:
      sku: E1M-V2N101
    libraries: [lwm2m]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.resolve_selection(project, liblayer.METADATA_ROOT)
    msg = str(exc.value)
    assert "lwm2m" in msg and "zephyr" in msg


# ---------------------------------------------------------------------
# ADR 0018 industrial connectivity + scripting additions
#
# Grounding (2026-07-07):
#   * CONFIG_MODBUS       $ZEPHYR_BASE/subsys/modbus/Kconfig `menuconfig MODBUS`
#   * CONFIG_CANOPENNODE  $ZEPHYR_BASE/modules/canopennode/Kconfig
#                         `config CANOPENNODE`
#   * canopennode west pin from $ZEPHYR_BASE/submanifests/optional.yaml
#   * micropython is not in the pinned workspace; exact source pin lives in
#     integration.zephyr.west and no CONFIG is invented.
# ---------------------------------------------------------------------

INDUSTRIAL_SCRIPTING_LIBS = {"modbus", "canopennode", "micropython"}


def test_industrial_scripting_manifests_present() -> None:
    on_disk = {p.stem for p in LIBRARIES_DIR.glob("*.yaml")}
    assert INDUSTRIAL_SCRIPTING_LIBS <= on_disk, (
        f"missing manifests: {INDUSTRIAL_SCRIPTING_LIBS - on_disk}")


def test_modbus_manifest_is_tier_a_in_tree() -> None:
    doc = yaml.safe_load((LIBRARIES_DIR / "modbus.yaml").read_text(encoding="utf-8"))
    assert doc["tier"] == "A"
    assert doc["version"] == "4.4.1"
    assert doc["license"] == "Apache-2.0"
    zephyr = doc["integration"]["zephyr"]
    assert zephyr.get("module") is None
    assert zephyr.get("kconfig") == ["CONFIG_MODBUS=y"]


def test_canopennode_manifest_records_optional_west_pin() -> None:
    doc = yaml.safe_load((LIBRARIES_DIR / "canopennode.yaml").read_text(encoding="utf-8"))
    assert doc["tier"] == "B"
    assert doc["license"] == "Apache-2.0"
    assert doc["version"] == "dec12fa3f0d790cafa8414a4c2930ea71ab72ffd"
    zephyr = doc["integration"]["zephyr"]
    assert zephyr.get("module") == "canopennode"
    assert zephyr.get("kconfig") == ["CONFIG_CANOPENNODE=y"]
    assert zephyr["west"]["name"] == "canopennode"
    assert zephyr["west"]["revision"] == "dec12fa3f0d790cafa8414a4c2930ea71ab72ffd"
    assert zephyr["west"]["path"] == "modules/lib/canopennode"


def test_micropython_manifest_is_module_only_source_pin() -> None:
    doc = yaml.safe_load((LIBRARIES_DIR / "micropython.yaml").read_text(encoding="utf-8"))
    assert doc["tier"] == "B"
    assert doc["version"] == "v1.24.1"
    assert doc["license"] == "MIT"
    zephyr = doc["integration"]["zephyr"]
    assert zephyr.get("module") == "micropython"
    assert zephyr["west"]["revision"] == "v1.24.1"
    assert zephyr["west"]["path"] == "modules/lib/micropython"
    assert "kconfig" not in zephyr, "no MicroPython Kconfig may be invented"


_V2N_MODBUS_CANOPEN = """
som:
  sku: E1M-V2N101
libraries: [modbus, canopennode]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_modbus_canopennode_zephyr_kconfig(tmp_path: Path) -> None:
    """The industrial libraries emit their grounded upstream enable symbols."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_MODBUS_CANOPEN))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_MODBUS=y" in out
    assert "CONFIG_CANOPENNODE=y" in out
    assert "modbus v4.4.1" in out
    assert "canopennode vdec12fa3f0d790cafa8414a4c2930ea71ab72ffd" in out


_V2N_MICROPYTHON = """
som:
  sku: E1M-V2N101
libraries: [micropython]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_micropython_module_only_no_kconfig(tmp_path: Path) -> None:
    """MicroPython emits a selection tag and no fabricated CONFIG line."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_MICROPYTHON))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "ADR 0018" in out
    assert "micropython v1.24.1" in out
    assert "west module `micropython`" in out
    assert "CONFIG_MICROPYTHON" not in out


def test_new_m_class_libraries_reject_a_only_soc() -> None:
    """The new Zephyr/Cortex-M entries name core_class on an A-only target."""
    project = BoardProject(
        sku="E1M-TST-AONLY",
        hw_rev=None,
        board_name=None,
        board_hw_rev=None,
        cores={"a0": Slice(core_id="a0", os="zephyr")},
        ipc=[],
        soc_spec={
            "cores": [{"id": "a0", "type": "cortex-a55"}],
            "soc_ram_kb": 4096,
        },
        som_preset={},
        board_preset=None,
    )
    for name in sorted(INDUSTRIAL_SCRIPTING_LIBS):
        manifest = yaml.safe_load((LIBRARIES_DIR / f"{name}.yaml").read_text(encoding="utf-8"))
        with pytest.raises(OrchestratorError) as exc:
            liblayer._check_requires(name, manifest, project, liblayer.METADATA_ROOT)
        assert name in str(exc.value)
        assert "core_class" in str(exc.value)


# ---------------------------------------------------------------------
# ONNX Runtime -- the A55 CPU inference floor (yocto-only, tier B)
# ---------------------------------------------------------------------

def test_onnxruntime_is_yocto_only_and_a_class():
    """ORT targets the A55 Linux side only. A zephyr section would imply an
    M-class build we do not ship."""
    manifest = yaml.safe_load(
        (REPO / "metadata" / "libraries" / "onnxruntime.yaml").read_text(encoding="utf-8")
    )
    assert manifest["tier"] == "B"
    assert "yocto" in manifest["integration"]
    assert "zephyr" not in manifest["integration"]
    assert manifest["requires"]["core_class"] == "a"
# Core-scoped libraries go through the SAME ADR-0018 layer as project-wide
# ones (alplabai/tan-cli#555).
#
# `loader._normalize_libraries` folds a `libraries:` entry carrying `cores:`
# into `cores[<id>]['libraries']` and leaves `project['libraries']` empty.
# That channel used to bypass the layer completely: no unknown-name refusal,
# no `requires:` check, no wireability check, and a Yocto slice INVENTED the
# package name as `lib-<name>` -- a recipe that RPROVIDES nothing.
# ---------------------------------------------------------------------

_CORE_SCOPED_LVGL_YOCTO = """
som:
  sku: E1M-V2N101
libraries:
  - name: lvgl
    cores: [a55_cluster]
cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
"""


def test_core_scoped_yocto_uses_the_manifest_recipe_name(tmp_path: Path) -> None:
    """A core-scoped `lvgl` emits lvgl.yaml's own
    `integration.yocto.image_install` -- never a `lib-`-prefixed invention."""
    project = load_board_yaml(_write_board(tmp_path, _CORE_SCOPED_LVGL_YOCTO))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert 'IMAGE_INSTALL:append = " lvgl"' in out
    assert "lib-lvgl" not in out


def test_core_scoped_yocto_reads_ros2_rclcpp(tmp_path: Path) -> None:
    """ros2's manifest names `rclcpp`, not its own library name."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: ros2
        cores: [a55_cluster]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert 'IMAGE_INSTALL:append = " rclcpp"' in out
    assert "lib-ros2" not in out


def test_core_scoped_library_with_no_yocto_section_emits_no_package(
        tmp_path: Path) -> None:
    """mbedtls' manifest has no `integration.yocto:` at all, so the honest
    emit is nothing -- surfaced as a comment, never as a fabricated recipe."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: mbedtls
        cores: [a55_cluster]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: zephyr
        app: ./m33
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert "lib-mbedtls" not in out
    assert "IMAGE_INSTALL:append" not in out
    assert "no `integration.yocto:` section" in out


def test_core_scoped_unknown_library_is_refused(tmp_path: Path) -> None:
    """An unknown core-scoped NAME raises the same self-correcting
    `load_manifest` error the project-wide form raises."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: not-a-library
        cores: [a55_cluster]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        _slice_local_conf(project, project.cores["a55_cluster"])
    msg = str(exc.value)
    assert "unknown library `not-a-library`" in msg
    assert "Available:" in msg


def test_project_wide_requires_still_fires_when_no_core_satisfies_it(
        tmp_path: Path) -> None:
    """The PROJECT-WIDE `requires:` check: ros2 needs `os: [yocto]`, and this
    board parks a55_cluster at `os: "off"`, so the project's whole live-OS set
    is {zephyr} and `_check_requires` refuses.

    This is what the original `test_core_scoped_requires_constraint_is_checked`
    actually measured.  It never exercised a per-core constraint at all -- with
    the A55 switched off, ANY declaration channel would have been refused by
    the project-wide check, so it passed identically with the per-slice guard
    absent.  The real core-scoping case is the next test.
    """
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: ros2
        cores: [m33_sm]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m33_sm"])
    msg = str(exc.value)
    assert "ros2" in msg
    assert "this project's cores run" in msg


def test_core_scoped_requires_constraint_is_checked(tmp_path: Path) -> None:
    """ros2 declares `requires: {os: [yocto], core_class: a}`.  Scoped to the
    Cortex-M Zephyr core of a REALISTIC V2N/V2M board -- yocto A55 *live*
    alongside the Zephyr M33 -- it must still fail naming the constraint.

    This is the shape the project-wide check cannot catch: `_project_oses`
    contains `yocto` from the A55, so `_check_requires` passes, and before the
    per-slice guard nothing else looked -- `_check_core_class` was reachable
    only from `zephyr_kconfig_lines` AFTER its `if not zephyr: continue`, and
    ros2's manifest has no `integration.zephyr:`.  Measured on the unfixed
    tree: no refusal at all, and the m33 alp.conf simply never mentioned ros2
    (alplabai/tan-cli#555).
    """
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: ros2
        cores: [m33_sm]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: yocto
        app: ./a55
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    # Precondition: the project-wide check IS satisfied -- the A55 runs yocto.
    assert "yocto" in liblayer._project_oses(project)
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m33_sm"])
    msg = str(exc.value)
    assert "ros2" in msg
    assert "m33_sm" in msg
    assert "yocto" in msg


def test_core_scoped_guard_also_fires_on_a_yocto_slice(
        tmp_path: Path) -> None:
    """The per-slice guard is not Zephyr-only.  micro-ros is the mirror image
    of ros2 -- the M-class / Zephyr client -- so scoping it to the A55 Yocto
    core has to be refused there too, on the Yocto emit path
    (`yocto_image_install` -> `resolve_selection(slice_=)`)."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: micro-ros
        cores: [a55_cluster]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: yocto
        app: ./a55
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        _slice_local_conf(project, project.cores["a55_cluster"])
    msg = str(exc.value)
    assert "micro-ros" in msg
    assert "a55_cluster" in msg


def test_core_scoped_library_with_no_section_for_this_os_is_not_refused(
        tmp_path: Path) -> None:
    """The per-slice guard checks `requires:`, NOT "is there an
    `integration.<os>:` section".  mbedtls and nlohmann-json are scoped to the
    Yocto cores of four shipped examples and neither manifest has an
    `integration.yocto:` section; the honest handling there is the explanatory
    `yocto_unwireable` comment, not a hard error.  Pinned so a later tightening
    of the guard cannot break those four boards."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - name: mbedtls
        cores: [a55_cluster]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: yocto
        app: ./a55
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert "lib-mbedtls" not in out
    assert "integration.yocto:" in out


def test_scoped_names_unions_both_declaration_channels(tmp_path: Path) -> None:
    """`scoped_names` is the one place both channels meet: project-wide names
    first, then the slice's core-scoped ones; `slice_=None` covers every
    core."""
    body = """
    som:
      sku: E1M-V2N101
    libraries:
      - lvgl
      - name: cmsis-dsp
        cores: [m33_sm]
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    assert liblayer.scoped_names(project) == ["lvgl", "cmsis-dsp"]
    assert liblayer.scoped_names(project, project.cores["m33_sm"]) == [
        "lvgl", "cmsis-dsp"]
    assert liblayer.scoped_names(project, project.cores["a55_cluster"]) == ["lvgl"]


def test_declaration_form_does_not_change_emitted_kconfig(tmp_path: Path) -> None:
    """#1359: the same library on the same core must emit the identical
    Kconfig set whether board.yaml spells the selection as a bare/project-
    wide entry or as a `cores:`-scoped one -- a `cores:` list reads as
    narrowing, and must never silently widen what gets configured.

    cmsis-dsp on an AEN401 m55_hp core is the issue's own repro: its
    manifest carries both a `hw_backends` accelerator matcher (HELIUM /
    ADC_DMA) and a `sw_fallback` floor (SCALAR) -- exactly the two derivers
    that used to read only the core-scoped channel."""
    bare = """
    som:
      sku: E1M-AEN401
    libraries: [cmsis-dsp]
    cores:
      m55_hp:
        os: zephyr
        app: ./src
    """
    scoped = """
    som:
      sku: E1M-AEN401
    libraries:
      - name: cmsis-dsp
        cores: [m55_hp]
    cores:
      m55_hp:
        os: zephyr
        app: ./src
    """
    bare_dir = tmp_path / "bare"
    scoped_dir = tmp_path / "scoped"
    bare_dir.mkdir()
    scoped_dir.mkdir()
    bare_project = load_board_yaml(_write_board(bare_dir, bare))
    scoped_project = load_board_yaml(_write_board(scoped_dir, scoped))
    bare_out = _slice_alp_conf(bare_project, bare_project.cores["m55_hp"])
    scoped_out = _slice_alp_conf(scoped_project, scoped_project.cores["m55_hp"])

    def cmsis_lines(out: str) -> set[str]:
        return {line.split("  #", 1)[0].strip()
                for line in out.splitlines() if "CMSIS_DSP" in line}

    bare_lines = cmsis_lines(bare_out)
    scoped_lines = cmsis_lines(scoped_out)
    assert bare_lines == scoped_lines
    # Pin the specific symbols #1359 measured as missing from the
    # project-wide form, so a future regression names exactly what broke.
    for symbol in (
        "CONFIG_ALP_CMSIS_DSP_SCALAR=y",
        "CONFIG_ALP_CMSIS_DSP_HELIUM=y",
        "CONFIG_ALP_CMSIS_DSP_ADC_DMA=y",
    ):
        assert symbol in bare_lines, f"missing {symbol} from project-wide form"


def test_declaration_form_does_not_change_the_inference_block(
    tmp_path: Path,
) -> None:
    """#1359 follow-up: `_slice_wants_inference` used to read only
    `slice_.libraries`, so a project-wide `libraries: [tflite-micro]`
    (no `cores:` key, no `cores.<id>.inference:` block) never tripped
    the inference-block emit, while the identical `cores:`-scoped
    spelling did -- the whole `_emit_inference` section (TFLM + Ethos-U
    dispatcher enables) went missing depending on how board.yaml spelled
    the same selection.

    E1M-AEN801 m55_hp is the issue's own repro: the SoM's Ethos-U55 +
    Ethos-U85 NPUs plus the M55's Helium MVE make this the same 13-line
    section the review measured as a superset."""
    bare = """
    som:
      sku: E1M-AEN801
    libraries: [tflite-micro]
    cores:
      m55_hp:
        app: ./src
    """
    scoped = """
    som:
      sku: E1M-AEN801
    libraries:
      - name: tflite-micro
        cores: [m55_hp]
    cores:
      m55_hp:
        app: ./src
    """
    bare_dir = tmp_path / "bare"
    scoped_dir = tmp_path / "scoped"
    bare_dir.mkdir()
    scoped_dir.mkdir()
    bare_project = load_board_yaml(_write_board(bare_dir, bare))
    scoped_project = load_board_yaml(_write_board(scoped_dir, scoped))
    bare_out = _slice_alp_conf(bare_project, bare_project.cores["m55_hp"])
    scoped_out = _slice_alp_conf(scoped_project, scoped_project.cores["m55_hp"])

    # Pin the specific symbols the review measured as the 13-line
    # superset the core-scoped form emitted and the project-wide form
    # silently dropped.
    for symbol in (
        "CONFIG_CPP=y",
        "CONFIG_STD_CPP17=y",
        "CONFIG_TENSORFLOW_LITE_MICRO=y",
        "CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y",
        "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y",
        "CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y",
        "CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y",
        "CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y",
        "CONFIG_ETHOS_U_DCACHE=y",
        "CONFIG_ETHOS_U85_256=y",
        "CONFIG_DCACHE=n",
        "CONFIG_HEAP_MEM_POOL_SIZE=65536",
    ):
        assert symbol in scoped_out, f"missing {symbol} from cores:-scoped form"
        assert symbol in bare_out, f"missing {symbol} from project-wide form"


# --- floating version pins are a supply-chain hole --------------------


def test_no_library_manifest_tracks_a_floating_branch() -> None:
    """A floating `main`/`master` pin is a supply-chain hole: the build is not
    reproducible and an upstream force-push silently changes what we ship.

    This is a DENYLIST of the specific floating refs seen in this repo
    (`main`, `master`, `HEAD`, `trunk`, empty string) -- it does not catch
    every moving branch a manifest could pin to. `micro-ros.yaml` and
    `ros2.yaml` both legitimately pin `version: humble`, a Zephyr/ROS 2
    release-codename branch that keeps moving; it is a known
    moving-branch pin that intentionally passes this guard. Tightening
    this to a shape rule (e.g. requiring a semver tag or full SHA) is a
    separate decision -- micro-ros/ros2 would need re-pinning first."""
    floating = {"main", "master", "HEAD", "trunk", ""}
    offenders = []
    for path in sorted((REPO / "metadata" / "libraries").glob("*.yaml")):
        manifest = yaml.safe_load(path.read_text(encoding="utf-8"))
        version = str(manifest.get("version", "")).strip()
        if version in floating or version.lower().startswith("unpinned"):
            offenders.append(path.name)
    assert offenders == [], f"floating version pins: {offenders}"


def test_no_west_manifest_extras_tier1_tracks_a_floating_branch() -> None:
    """Same hole, at the west.yml source of truth: a project in the
    ``extras-tier1`` group must pin a tag or commit SHA, never a branch --
    a floating branch pin can also silently not exist at all (minimp3's
    prior ``main`` pin: the branch never existed on lieff/minimp3).

    Like its manifest-layer sibling above, this is a DENYLIST of the
    specific floating refs seen in this repo (`main`, `master`, `HEAD`,
    `trunk`, empty string), not a shape rule -- it does not catch every
    moving branch a west revision could name (a release codename like
    `humble` would pass here too, same as it does for the manifest-layer
    `version:` field checked above -- see that test's docstring).
    Tightening this to a shape rule is a separate decision that would
    require re-pinning micro-ros/ros2 first."""
    floating = {"main", "master", "HEAD", "trunk", ""}
    doc = yaml.safe_load((REPO / "west.yml").read_text(encoding="utf-8"))
    offenders = []
    for project in doc["manifest"]["projects"]:
        if "extras-tier1" not in (project.get("groups") or []):
            continue
        revision = str(project.get("revision", "")).strip()
        if revision in floating:
            offenders.append(f"{project['name']}: {revision!r}")
    assert offenders == [], f"floating extras-tier1 revisions: {offenders}"


def test_nightly_extras_tier1_workflow_does_not_hardcode_the_library_list() -> None:
    """`.github/workflows/nightly-extras-tier1-pins.yml` must DERIVE its
    extras-tier1 roster from west.yml at run time, not hardcode it as a
    literal list of project names / `modules/lib/` path basenames in a
    step's `run:` body.

    This is not hypothetical: PR #1237 added three libraries to west.yml's
    extras-tier1 group (cmsisstream, CMSIS-CV, Arm-2D) and did NOT update
    this workflow's then-hardcoded `west update` argument list or verify-loop
    library list -- so those three were fetched and checked by nothing. That
    is exactly the "a pin nothing checks" failure class this workflow exists
    to close. If this test fails, someone re-hardcoded the roster here --
    derive it from west.yml's `extras-tier1` group in a workflow step
    instead of re-adding names/paths as literal tokens.

    Tokenizing splits on commas as well as whitespace: a re-hardcoded list
    disguised as a single comma-joined token (e.g. ``for lib in $(echo
    "u8g2,libcoap,...,arm-2d" | tr , " ")``) must still be caught. The
    3-in-a-row threshold below is deliberately low: re-hardcoding only 1-2
    library names/paths (a one-off example in a comment, an unrelated
    coincidental match) does NOT trip this guard -- 3 is the line between
    "coincidence" and "someone pasted the roster back in".
    """
    west_doc = yaml.safe_load((REPO / "west.yml").read_text(encoding="utf-8"))
    known_tokens = set()
    for project in west_doc["manifest"]["projects"]:
        if "extras-tier1" not in (project.get("groups") or []):
            continue
        known_tokens.add(project["name"])
        known_tokens.add(project["path"].rsplit("/", 1)[-1])

    workflow_path = REPO / ".github" / "workflows" / "nightly-extras-tier1-pins.yml"
    workflow = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))

    offenders = []
    for step in workflow["jobs"]["fetch-and-verify"]["steps"]:
        run = step.get("run")
        if not run:
            continue
        # Fold backslash line-continuations -- both original hardcoded
        # lists were spread across several continuation lines -- so a
        # re-hardcoded list still reads as one run of tokens. Also split
        # on commas: a comma-joined roster (e.g. piped through `tr , " "`
        # at runtime) is a single whitespace token but must tokenize the
        # same as a space-separated one.
        folded = run.replace("\\\n", " ").replace(",", " ")
        run_len = 0
        for token in folded.split():
            token = token.strip(",;")
            if token in known_tokens:
                run_len += 1
                if run_len == 3:
                    offenders.append((step.get("name"), token))
            else:
                run_len = 0
    assert offenders == [], (
        "nightly-extras-tier1-pins.yml hardcodes 3+ extras-tier1 library "
        f"names/paths again: {offenders}. This is the PR #1237 recurrence "
        "(three libraries added to west.yml's extras-tier1 group were "
        "never fetched or verified because the workflow's list was "
        "hardcoded) -- derive the roster from west.yml in a workflow step "
        "instead of hardcoding it."
    )


def test_nightly_extras_tier1_workflow_verify_step_fails_closed_on_empty_roster() -> (
    None
):
    """The "Derive extras-tier1 library list from west.yml" step
    (``id: extras-tier1``) must exist, and both steps that consume its
    outputs must actually reference ``steps.extras-tier1.outputs.*``.

    Not hypothetical: ``bash -c 'set -euo pipefail; status=0; for lib in
    ; do echo "$lib"; done; exit $status'`` exits 0 -- an empty
    ``${{ steps.extras-tier1.outputs.paths }}`` (the derive step deleted,
    renamed, or its ``id:`` typo'd) makes the "Verify pins populated
    content" step's `for` loop iterate zero times and pass, gating on
    having checked nothing. Asserting the id: exists and is wired into
    both consumer steps catches that at review time; the workflow's own
    `[ -n "$libs" ] || exit 1` guard catches it at run time.
    """
    workflow_path = REPO / ".github" / "workflows" / "nightly-extras-tier1-pins.yml"
    workflow = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))
    steps = workflow["jobs"]["fetch-and-verify"]["steps"]

    derive_steps = [s for s in steps if s.get("id") == "extras-tier1"]
    assert len(derive_steps) == 1, (
        "nightly-extras-tier1-pins.yml is missing the `id: extras-tier1` "
        "derive step (or it was renamed) -- the west-update and verify "
        "steps below depend on steps.extras-tier1.outputs.{names,paths} "
        "existing, and an empty output there makes the verify loop pass "
        "having checked nothing."
    )

    update_step = next(
        s
        for s in steps
        if s.get("name") == "West init + update with extras-tier1 enabled"
    )
    verify_step = next(
        s for s in steps if s.get("name") == "Verify pins populated content"
    )

    update_refs = " ".join(str(v) for v in (update_step.get("env") or {}).values())
    verify_refs = " ".join(str(v) for v in (verify_step.get("env") or {}).values())
    assert "steps.extras-tier1.outputs.names" in update_refs, (
        "the west-update step must reference steps.extras-tier1.outputs.names"
    )
    assert "steps.extras-tier1.outputs.paths" in verify_refs, (
        "the verify step must reference steps.extras-tier1.outputs.paths"
    )
