# SPDX-License-Identifier: Apache-2.0
"""The #2277 runtime-sync fix must stay in sync between its real sources and
their host-buildable test mirror.

`tests/unit/isp_ae_calib_envelope_sync/src/test_isp_ae_calib_envelope_sync.c`
mirrors alp-sdk-owned pure helpers directly from `zephyr/drivers/video/
isp_pico.c` (not a vendored patch) -- brace-matched extraction, byte-for-byte
(whitespace aside) -- plus one field-set check against the vendored
`zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-envelope.patch`'s
`isp_vsi_sync_ae_sns_default()` (see that file's own comment for the full
rationale). An earlier field-set check against a since-removed
`isp_calib_param.modules.ae.autoAttr` mirror, and a presence check for a
since-removed AE-reinit cycle in `isp_vsi_sync_ae_sns_default()`, were
deleted alongside the dead code they covered (bench run 251 -- see the
patch's own comment).
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

# #2277 (bench run 245, fourth cut): isp_pico.c's third pure helper -- the
# ISP's own frame-rate diagnostic, added alongside the AE-reinit/clamp fix
# below so a bench readback can tell whether the library honoured the
# re-init or the clamp caught it.
FPS_X100_FUNC_SIGNATURE = re.compile(r"^static uint32_t isp_ae_fps_x100_from_frame_delta\(")


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
    """hal_alif patch 0011's isp_vsi_sync_ae_sns_default() assigns
    sensor_attributes.{fullLinesStd,fullLines,maxIntLine} -- the struct
    vsi_int_time_update()'s write-back clamp reads from (bench run 251:
    the actual enforcement point). Field-path-SET check against the
    mirror's isp_sns_default_sync()."""
    patch_fields = _assigned_fields(
        line for line in _patch_added_lines() if "sensor_attributes." in line
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(),
        re.compile(r"^(static void\s+)?isp_sns_default_sync\("),
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


def test_fps_helper_matches_isp_pico_verbatim() -> None:
    """#2277 (bench run 245, fourth cut): isp_pico.c is alp-sdk source, same
    status as the two frmival helpers above -- read it directly."""
    real_body = _extract_function_body(
        ISP_PICO.read_text(encoding="utf-8").splitlines(), FPS_X100_FUNC_SIGNATURE
    )
    mirror_body = _extract_function_body(
        MIRROR.read_text(encoding="utf-8").splitlines(), FPS_X100_FUNC_SIGNATURE
    )

    assert _normalise(real_body) == _normalise(mirror_body), (
        "isp_ae_fps_x100_from_frame_delta() has drifted between "
        f"{ISP_PICO.relative_to(REPO)} and its host-buildable test mirror "
        f"{MIRROR.relative_to(REPO)}.\n\n"
        f"--- real body ---\n{real_body}\n\n"
        f"--- mirror body ---\n{mirror_body}"
    )


def _patch_reconstructed_file_lines(path_suffix: str) -> list[str]:
    """Reconstruct one file's POST-patch text (context + added lines, diff
    prefix stripped, removed lines dropped) from the unified diff -- unlike
    _patch_added_lines(), this keeps UNCHANGED lines a hunk carries as
    context (e.g. an unchanged function signature/opening brace around a
    changed body), which a bare '+'-only scan silently drops."""
    raw = CALIB_PATCH.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(raw) if line == f"+++ b/{path_suffix}") + 1

    lines = []
    for line in raw[start:]:
        if line.startswith("--- ") or line.startswith("+++ "):
            break  # next file section of the patch
        if line.startswith("@@") or line.startswith("-"):
            continue
        if line.startswith("+") or line.startswith(" "):
            lines.append(line[1:])
    return lines


def _patch_function_body(func_signature_substr: str) -> str:
    """Brace-matched extraction from isp_api_wrapper.c's POST-patch text --
    same technique as _extract_function_body(), but over
    _patch_reconstructed_file_lines() (this file's own diff-hunk text, not
    a real source file) since 0011 is a unified diff, not a standalone .c
    file."""
    lines = _patch_reconstructed_file_lines("drivers/isp/isp_wrapper/src/isp_api_wrapper.c")
    start = next(i for i, line in enumerate(lines) if func_signature_substr in line)

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
    assert end is not None, f"unbalanced braces extracting {func_signature_substr!r} from patch"

    return "\n".join(lines[start : end + 1])


def test_sns_default_sync_does_not_reach_into_the_library() -> None:
    """Bench run 251: re-registering aeSnsFunc with the library
    (VSI_MPI_ISP_InitAeSnsFunc()) and cycling VSI_MPI_ISP_AeUnRegCallBack()/
    AeRegCallBack() were both tried and both proven to have no effect on
    the library's own internal AE request (it stays pinned at the
    compiled-in boot default regardless) -- isp_vsi_sync_ae_sns_default()
    was simplified back to the plain sensor_attributes field sync
    vsi_int_time_update()'s write-back clamp actually reads from. This
    fails if any of those vendor callback-registration calls reappear
    without new bench evidence that they do something after all."""
    body = _patch_function_body("int isp_vsi_sync_ae_sns_default(")

    for call in ("VSI_MPI_ISP_InitAeSnsFunc", "VSI_MPI_ISP_AeUnRegCallBack",
                 "VSI_MPI_ISP_AeRegCallBack"):
        assert call not in body, (
            f"isp_vsi_sync_ae_sns_default() calls {call} again -- bench run 251 proved "
            "this registration dance has no effect on the library's own internal AE "
            "request (it stays pinned at the compiled-in boot default regardless); "
            "don't re-add it without new bench evidence."
        )


def test_int_time_write_clamps_to_active_max_int_line() -> None:
    """Bench run 251: vsi_int_time_update() (aeSnsFunc.pfnIntTimeUpdate,
    the closed library's own per-frame intLine write-back callback -- the
    LAST touchpoint before the value reaches the sensor) is THE mechanism
    bench-proven to hold the sensor within the active-mode ceiling -- it
    must clamp against sensor_attributes.maxIntLine, the same ceiling
    isp_vsi_sync_ae_sns_default() writes."""
    body = _patch_function_body("static int vsi_int_time_update(")

    assert "sensor_attributes.maxIntLine" in body, (
        "vsi_int_time_update() no longer references sensor_attributes.maxIntLine -- "
        "the bench-proven enforcement point (bench run 251) is gone."
    )
    assert re.search(r"\*pIntLine\s*=", body), (
        "vsi_int_time_update() must write the CLAMPED value back through *pIntLine "
        "(the library reads this pointer back after the callback returns), not just "
        "clamp a local copy that never reaches the sensor."
    )
