# SPDX-License-Identifier: Apache-2.0
"""The #2277 runtime-sync fix must stay in sync between its two real sources
and their host-buildable test mirror.

`tests/unit/isp_ae_calib_envelope_sync/src/test_isp_ae_calib_envelope_sync.c`
mirrors TWO pieces of the real fix, from two different kinds of source (see
that file's own comment for the full rationale):

  1. `isp_ae_int_time_max_us_from_frmival()` -- alp-sdk-owned, in
     `zephyr/drivers/video/isp_pico.c` directly (not a vendored patch).
     Checked the same way `test_isp_ae_ctrl_clamp_patch_mirror.py` checks a
     vendored patch's function: brace-matched extraction, byte-for-byte
     (whitespace aside).

  2. `isp_calib_ae_envelope_sync()` -- a conceptual mirror of three field-
     copy lines `zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-envelope
     .patch` adds to vendored, non-host-buildable `isp_api_wrapper.c` (it
     calls into the closed VSI_MPI_ISP library and needs vendor struct
     types the test cannot include). Unlike case 1, this cannot be a
     byte-for-byte match: the real code assigns through
     `isp_calib_param.modules.ae.autoAttr.X = exp_attr.autoAttr.X` inline
     inside a much larger vendor-typed function, the mirror is its own
     small function over a local POD struct. What must NOT drift is the
     SET of fields mirrored (`autoAttr.{expTimeRange,againRange,
     dgainRange}`) -- an edit that adds/drops a field on one side without
     the other silently changes what #2277 actually keeps in sync, green
     the whole time. This test extracts that field-name set from each
     source's assignment statements and fails if they differ.
"""

from __future__ import annotations

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
ISP_PICO = REPO / "zephyr" / "drivers" / "video" / "isp_pico.c"
CALIB_PATCH = REPO / "zephyr" / "patches" / "hal_alif" / "0011-isp-ov5647-ae-calib-envelope.patch"
MIRROR = (
    REPO
    / "tests"
    / "unit"
    / "isp_ae_calib_envelope_sync"
    / "src"
    / "test_isp_ae_calib_envelope_sync.c"
)

FRMIVAL_FUNC_SIGNATURE = re.compile(
    r"^static uint32_t isp_ae_int_time_max_us_from_frmival\("
)

# #2277 (third cut): isp_pico.c's OTHER pure helper, added alongside
# isp_ae_int_time_max_us_from_frmival() above but deriving the sensor
# DEFAULT'S line ceiling (VTS) instead of the microsecond exposure range --
# see that file's own comment for why this, not the microsecond range, is
# what bench run 244 found actually bounds the library's intLine output.
SNS_FULL_LINES_FUNC_SIGNATURE = re.compile(
    r"^static uint32_t isp_ae_sns_full_lines_from_frmival\("
)


def _extract_function_body(lines: list[str], signature: re.Pattern[str]) -> str:
    """Return signature-through-closing-brace text, brace-depth walked (same
    technique as test_isp_ae_ctrl_clamp_patch_mirror.py)."""
    start = next(i for i, line in enumerate(lines) if signature.match(line))

    depth = 0
    opened = False
    end = None
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            opened = True
        if opened and depth == 0:
            end = i
            break
    assert end is not None, f"unbalanced braces extracting {signature.pattern}"

    return "\n".join(lines[start : end + 1])


def _normalise(text: str) -> str:
    return "\n".join(" ".join(line.split()) for line in text.splitlines() if line.strip())


def test_frmival_helper_matches_isp_pico_verbatim() -> None:
    """isp_pico.c is alp-sdk source, not a patch -- read it directly."""
    real_body = _extract_function_body(
        ISP_PICO.read_text(encoding="utf-8").splitlines(), FRMIVAL_FUNC_SIGNATURE
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(), FRMIVAL_FUNC_SIGNATURE
    )

    assert _normalise(real_body) == _normalise(mirror_body), (
        "isp_ae_int_time_max_us_from_frmival() has drifted between "
        f"{ISP_PICO.relative_to(REPO)} and its host-buildable test mirror "
        f"{MIRROR.relative_to(REPO)}.\n\n"
        f"--- real body ---\n{real_body}\n\n"
        f"--- mirror body ---\n{mirror_body}"
    )


# One assignment statement's target field-path, e.g.
# "isp_calib_param.modules.ae.autoAttr.expTimeRange = exp_attr.autoAttr.expTimeRange;"
# -> "expTimeRange". Matches both the real (dotted, vendor-typed) and mirror
# (arrow, POD-typed) spellings -- only the trailing field name after the
# LAST '.'/'->'  before '=' is compared, not the whole LHS expression.
ASSIGNMENT_FIELD = re.compile(r"[.>]\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[A-Za-z_]")


def _assigned_fields(lines: list[str]) -> set[str]:
    """Field names, case/underscore-folded (`expTimeRange` == `exp_time_range`)
    -- the vendor struct is camelCase, the mirror's local POD struct is
    snake_case (this repo's own style for new alp-sdk C); only WHICH field is
    assigned is the invariant under test, not its naming convention."""
    return {
        m.group(1).replace("_", "").lower()
        for line in lines
        for m in ASSIGNMENT_FIELD.finditer(line)
    }


def _patch_added_lines() -> list[str]:
    lines = []
    for raw in CALIB_PATCH.read_text(encoding="utf-8").splitlines():
        if raw.startswith("+++ "):
            continue
        if raw.startswith("+"):
            lines.append(raw[1:])
    return lines


def test_calib_sync_mirrors_the_same_fields_as_the_patch() -> None:
    patch_fields = _assigned_fields(
        line for line in _patch_added_lines() if "isp_calib_param.modules.ae." in line
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(),
        re.compile(r"^static void isp_calib_ae_envelope_sync\("),
    )
    mirror_fields = _assigned_fields(mirror_body.splitlines())

    assert patch_fields, (
        f"found no isp_calib_param.modules.ae.* assignment in {CALIB_PATCH.relative_to(REPO)} "
        "-- did the #2277 sync mechanism move or get renamed?"
    )
    assert patch_fields == mirror_fields, (
        "The #2277 calibration-envelope sync mirrors a DIFFERENT set of fields than "
        f"{CALIB_PATCH.relative_to(REPO)} actually assigns.\n"
        f"patch fields:  {sorted(patch_fields)}\n"
        f"mirror fields: {sorted(mirror_fields)}\n"
        "Update BOTH copies together -- this test only compares WHICH fields are kept "
        "in sync, not the exact vendor-typed vs. POD-typed assignment syntax (see this "
        "file's own comment for why)."
    )


def test_sns_full_lines_helper_matches_isp_pico_verbatim() -> None:
    """#2277 (third cut): isp_ae_sns_full_lines_from_frmival() is alp-sdk
    source (isp_pico.c), same status as isp_ae_int_time_max_us_from_frmival()
    above -- read it directly, byte-for-byte (whitespace aside)."""
    real_body = _extract_function_body(
        ISP_PICO.read_text(encoding="utf-8").splitlines(), SNS_FULL_LINES_FUNC_SIGNATURE
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(), SNS_FULL_LINES_FUNC_SIGNATURE
    )

    assert _normalise(real_body) == _normalise(mirror_body), (
        "isp_ae_sns_full_lines_from_frmival() has drifted between "
        f"{ISP_PICO.relative_to(REPO)} and its host-buildable test mirror "
        f"{MIRROR.relative_to(REPO)}.\n\n"
        f"--- real body ---\n{real_body}\n\n"
        f"--- mirror body ---\n{mirror_body}"
    )


def test_sns_default_sync_mirrors_the_same_fields_as_the_patch() -> None:
    """#2277 (third cut): hal_alif patch 0011's new isp_vsi_sync_ae_sns_
    default() assigns sensor_attributes.{fullLinesStd,fullLines,maxIntLine}
    -- the struct actually registered with the library at
    VSI_MPI_ISP_InitAeSnsFunc() (bench run 244's root cause). Same
    field-path-SET check as test_calib_sync_mirrors_the_same_fields_as_the_
    patch() above, against the mirror's isp_sns_default_sync()."""
    patch_fields = _assigned_fields(
        line for line in _patch_added_lines() if "sensor_attributes." in line
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(),
        re.compile(r"^static void isp_sns_default_sync\("),
    )
    mirror_fields = _assigned_fields(mirror_body.splitlines())

    assert patch_fields, (
        f"found no sensor_attributes.* assignment in {CALIB_PATCH.relative_to(REPO)} -- "
        "did the #2277 (third cut) sync mechanism move or get renamed?"
    )
    assert patch_fields == mirror_fields, (
        "The #2277 (third cut) sensor-default sync mirrors a DIFFERENT set of fields "
        f"than {CALIB_PATCH.relative_to(REPO)} actually assigns.\n"
        f"patch fields:  {sorted(patch_fields)}\n"
        f"mirror fields: {sorted(mirror_fields)}\n"
        "Update BOTH copies together."
    )
