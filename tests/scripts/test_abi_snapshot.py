# SPDX-License-Identifier: Apache-2.0
"""
Tests for scripts/abi_snapshot.py -- the ABI extractor / freeze-gate
input.

Issue #624: the pre-fix parser flushed a multi-line struct/union/enum
typedef at its FIRST member's `;` (line-based flattening), so most
anonymous `typedef struct { ... } name;` declarations never got
recorded under `name` at all -- their first field name (e.g. `bus_id`)
was recorded as a bogus typedef instead, and the real struct's layout
was invisible to the freeze gate.  These tests pin the fixed,
brace-depth-aware behaviour: every anonymous/tagged public struct,
union, and enum is captured under its declared name with its full,
ordered member list, and a field add/remove/reorder/retype changes
both that member list and the record's hash (i.e. `--diff` reports it
as CHANGED).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import abi_snapshot as abi  # noqa: E402


def _extract_src(tmp_path: Path, src: str, name: str = "fixture.h"):
    path = tmp_path / name
    path.write_text(src, encoding="utf-8")
    return abi.extract(path)


# ---------------------------------------------------------------------
# Core bug: multiline anonymous struct typedefs
# ---------------------------------------------------------------------


def test_multiline_anonymous_struct_captured_under_typedef_name(tmp_path):
    src = """\
#ifndef FIXTURE_H
#define FIXTURE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t bus_id;
	uint32_t bitrate_hz;
} alp_i2c_config_t;

#ifdef __cplusplus
}
#endif
#endif
"""
    result = _extract_src(tmp_path, src)
    assert "alp_i2c_config_t" in result["typedefs"]
    rec = result["typedefs"]["alp_i2c_config_t"]
    assert rec["kind"] == "struct"
    assert rec["fields"] == ["uint32_t bus_id", "uint32_t bitrate_hz"]

    # The historical bug: the first member leaking out as its own typedef.
    assert "bus_id" not in result["typedefs"]


def test_one_line_struct_typedef_also_captured(tmp_path):
    src = "typedef struct { uint32_t a; uint32_t b; } alp_x_t;\n"
    result = _extract_src(tmp_path, src)
    assert result["typedefs"]["alp_x_t"]["fields"] == ["uint32_t a", "uint32_t b"]


def test_tagged_struct_typedef_with_body(tmp_path):
    src = """\
typedef struct alp_backend {
	const char *silicon_ref;
	uint8_t     priority;
} alp_backend_t;
"""
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_backend_t"]
    assert rec["kind"] == "struct"
    assert rec["fields"] == ["const char *silicon_ref", "uint8_t priority"]


def test_anonymous_enum_captured_with_enumerators(tmp_path):
    src = "typedef enum { ALP_A = 0, ALP_B = 1, ALP_C = 2 } alp_e_t;\n"
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_e_t"]
    assert rec["kind"] == "enum"
    assert rec["enumerators"] == ["ALP_A = 0", "ALP_B = 1", "ALP_C = 2"]


def test_nested_anonymous_union_inside_struct(tmp_path):
    src = """\
typedef struct {
	uint8_t kind;
	union {
		uint32_t as_u32;
		float    as_f32;
	} u;
} alp_variant_t;
"""
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_variant_t"]
    assert rec["kind"] == "struct"
    assert rec["fields"][0] == "uint8_t kind"
    assert rec["fields"][1].startswith("union { uint32_t as_u32")
    assert rec["fields"][1].endswith("} u")


def test_forward_declared_struct_gets_opaque_kind(tmp_path):
    src = "typedef struct alp_i2c alp_i2c_t;\n"
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_i2c_t"]
    assert rec["kind"] == "opaque"


def test_forward_decl_body_defined_later_merges_into_named_typedef(tmp_path):
    # include/alp/chips/cc3501e.h's shape: forward-declare, then define
    # the real (embeddable-by-value) body later in the same header.
    src = """\
typedef struct cc3501e cc3501e_t;

struct cc3501e {
	bool     initialised;
	uint8_t  rx_scratch[16];
};
"""
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["cc3501e_t"]
    assert rec["kind"] == "struct"
    assert rec["fields"] == ["bool initialised", "uint8_t rx_scratch[16]"]
    # No leftover placeholder under the bare tag.
    assert "struct cc3501e" not in result["typedefs"]


def test_function_pointer_typedef(tmp_path):
    src = "typedef void (*alp_cb_t)(uint8_t cmd, const uint8_t *payload, size_t len);\n"
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_cb_t"]
    assert rec["kind"] == "fnptr"


def test_function_pointer_field_inside_struct_captured_raw(tmp_path):
    src = """\
typedef struct {
	int (*probe)(uint32_t instance_id, uint32_t *refined_caps);
} alp_backend_t;
"""
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_backend_t"]
    assert rec["fields"] == ["int (*probe)(uint32_t instance_id, uint32_t *refined_caps)"]


def test_simple_alias_typedef(tmp_path):
    src = "typedef uint16_t alp_handle_t;\n"
    result = _extract_src(tmp_path, src)
    assert result["typedefs"]["alp_handle_t"]["kind"] == "alias"


def test_attribute_decorated_struct_typedef(tmp_path):
    src = (
        "typedef struct {\n"
        "\tuint32_t a;\n"
        "} __attribute__((packed)) alp_packed_t;\n"
    )
    result = _extract_src(tmp_path, src)
    rec = result["typedefs"]["alp_packed_t"]
    assert rec["kind"] == "struct"
    assert rec["fields"] == ["uint32_t a"]


def test_extern_c_and_include_guard_are_transparent(tmp_path):
    src = """\
#ifndef FIXTURE_H
#define FIXTURE_H
#ifdef __cplusplus
extern "C" {
#endif

void alp_a(void);
void alp_b(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* FIXTURE_H */
"""
    result = _extract_src(tmp_path, src)
    assert set(result["functions"]) == {"alp_a", "alp_b"}


def test_static_inline_function_definition_does_not_break_flattening(tmp_path):
    # A `static inline` helper's body has no trailing `;` -- its
    # closing brace must end the declaration, not glue every following
    # declaration into one blob (regression: earlier iteration of this
    # fix mis-stripped the extern "C" close and swallowed the rest of
    # the header into ssd1331_rgb565's "declaration").
    src = """\
#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t alp_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

typedef struct {
	uint8_t *fb;
} alp_display_t;

alp_status_t alp_display_init(alp_display_t *dev);

#ifdef __cplusplus
}
#endif
"""
    result = _extract_src(tmp_path, src)
    assert set(result["functions"]) == {"alp_rgb565", "alp_display_init"}
    assert result["typedefs"]["alp_display_t"]["fields"] == ["uint8_t *fb"]


def test_static_assert_is_ignored_not_misfiled_as_a_function(tmp_path):
    src = (
        "typedef struct { uint32_t a; } alp_x_t;\n"
        "_Static_assert(sizeof(alp_x_t) == 4, \"alp_x_t must be 4 bytes\");\n"
    )
    result = _extract_src(tmp_path, src)
    assert result["functions"] == {}


def test_extern_variable_declaration_captured(tmp_path):
    src = "extern const uint32_t alp_route_count;\nextern const uint8_t alp_routes[];\n"
    result = _extract_src(tmp_path, src)
    assert set(result["variables"]) == {"alp_route_count", "alp_routes"}


def test_unrecognisable_declaration_raises_with_file_context(tmp_path):
    path = tmp_path / "bad.h"
    path.write_text("weird_construct <<< 1;\n", encoding="utf-8")
    with pytest.raises(abi.AbiParseError) as exc:
        abi.extract(path)
    assert "bad.h" in str(exc.value)


def test_unbalanced_braces_raise_instead_of_silently_truncating(tmp_path):
    path = tmp_path / "unbalanced.h"
    path.write_text("typedef struct { uint32_t a;\n", encoding="utf-8")
    with pytest.raises(abi.AbiParseError):
        abi.extract(path)


# ---------------------------------------------------------------------
# Layout-change detection (the other acceptance criterion: reorder /
# retype / add / remove must all show up in `--diff`)
# ---------------------------------------------------------------------


def _snap(tmp_path, src, header="fixture.h"):
    return {"headers": {header: _extract_src(tmp_path, src, header)}}


def test_diff_flags_field_type_change(tmp_path):
    prev = _snap(
        tmp_path,
        "typedef struct { uint32_t bus_id; uint32_t bitrate_hz; } alp_i2c_config_t;\n",
    )
    curr = _snap(
        tmp_path,
        "typedef struct { uint32_t bus_id; uint64_t bitrate_hz; } alp_i2c_config_t;\n",
        header="fixture2.h",
    )
    # Compare same header key so diff() treats it as one file.
    curr = {"headers": {"fixture.h": curr["headers"]["fixture2.h"]}}
    msgs = abi.diff(prev, curr)
    assert any(m.startswith("CHANGED typedef") for m in msgs)
    assert any("bitrate_hz" in m and "uint32_t" in m and "uint64_t" in m for m in msgs)


def test_diff_flags_field_reorder(tmp_path):
    prev = _snap(tmp_path, "typedef struct { uint32_t a; uint32_t b; } alp_x_t;\n")
    curr_extract = _extract_src(
        tmp_path, "typedef struct { uint32_t b; uint32_t a; } alp_x_t;\n", "fixture2.h"
    )
    curr = {"headers": {"fixture.h": curr_extract}}
    msgs = abi.diff(prev, curr)
    assert any(m.startswith("CHANGED typedef") for m in msgs)


def test_diff_flags_field_add_and_remove(tmp_path):
    prev = _snap(tmp_path, "typedef struct { uint32_t a; } alp_x_t;\n")
    curr_extract = _extract_src(
        tmp_path, "typedef struct { uint32_t a; uint32_t b; } alp_x_t;\n", "fixture2.h"
    )
    curr = {"headers": {"fixture.h": curr_extract}}
    msgs = abi.diff(prev, curr)
    assert any(m.startswith("CHANGED typedef") for m in msgs)


def test_diff_is_clean_when_nothing_changed(tmp_path):
    src = "typedef struct { uint32_t a; uint32_t b; } alp_x_t;\n"
    prev = _snap(tmp_path, src)
    curr_extract = _extract_src(tmp_path, src, "fixture2.h")
    curr = {"headers": {"fixture.h": curr_extract}}
    assert abi.diff(prev, curr) == []


def test_mutation_alp_i2c_config_bitrate_hz_type_change_is_reported_changed():
    """The issue's own mutation test: change ONLY
    alp_i2c_config_t.bitrate_hz from uint32_t to uint64_t in the real
    header and confirm `--diff` reports it CHANGED."""
    header = REPO / "include" / "alp" / "peripheral.h"
    before = abi.extract(header)
    real_src = header.read_text(encoding="utf-8")
    mutated_src = real_src.replace(
        "uint32_t bitrate_hz;", "uint64_t bitrate_hz;", 1
    )
    assert mutated_src != real_src, "fixture assumption broke: bitrate_hz member text moved"

    import tempfile

    with tempfile.TemporaryDirectory() as d:
        mutated_path = Path(d) / "peripheral.h"
        mutated_path.write_text(mutated_src, encoding="utf-8")
        after = abi.extract(mutated_path)

    prev = {"headers": {"alp/peripheral.h": before}}
    curr = {"headers": {"alp/peripheral.h": after}}
    msgs = abi.diff(prev, curr)
    assert any(
        m == "CHANGED typedef alp/peripheral.h::alp_i2c_config_t" for m in msgs
    )
    assert any("bitrate_hz" in m for m in msgs)


# ---------------------------------------------------------------------
# Whole-repo regression guards
# ---------------------------------------------------------------------


def test_real_headers_all_parse_without_error():
    """Every committed public header must be classifiable -- an
    unrecognised declaration is a hard failure (see
    test_unrecognisable_declaration_raises_with_file_context), so this
    doubles as "the fixed parser covers this codebase's full style"."""
    snapshot = abi.build_snapshot("test", abi.INCLUDE_ROOT)
    assert len(snapshot["headers"]) > 100  # sanity: we actually walked the tree


def test_no_struct_member_identifier_leaks_as_a_bogus_typedef():
    """Regression guard for the exact defect in #624: none of the
    known first-member names that used to leak out as synthetic
    typedefs should appear as top-level typedef keys any more."""
    snapshot = abi.build_snapshot("test", abi.INCLUDE_ROOT)
    known_bogus_names = {"bus_id", "port_id", "keepalive_s", "id"}
    for header, body in snapshot["headers"].items():
        leaked = known_bogus_names & set(body.get("typedefs", {}))
        assert not leaked, f"{header}: bogus field-name typedef(s) {leaked}"


def test_previously_broken_public_structs_now_captured_with_fields():
    snapshot = abi.build_snapshot("test", abi.INCLUDE_ROOT)
    expected = {
        "alp_i2c_config_t": "alp/peripheral.h",
        "alp_uart_config_t": "alp/peripheral.h",
        "alp_spi_config_t": "alp/peripheral.h",
        "alp_camera_config_t": "alp/camera.h",
        "alp_camera_frame_t": "alp/camera.h",
        "alp_mqtt_config_t": "alp/iot.h",
        "alp_can_config_t": "alp/can.h",
    }
    for name, header in expected.items():
        rec = snapshot["headers"][header]["typedefs"].get(name)
        assert rec is not None, f"{name} missing from {header}"
        assert rec["kind"] == "struct"
        assert rec["fields"], f"{name} captured with no fields"
