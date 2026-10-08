# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Configure-time guard: fail the build when a driver alp-sdk ships depends on a
# `zephyr/patches.yml` patch that this workspace does not carry (#2766).
#
# Why: `west update` / `west build` never apply those patches, and the build
# still SUCCEEDS without them.  E.g. without patch zephyr/0001 the upstream
# clock_control_alif.c has no CSI pixel-clock set_rate, so the camera builds
# fine and then dies at runtime with `Failed to set CSI pixel clock rate!
# ret - -134` -> ALP_ERR_NOSUPPORT, which reads as a hardware fault.
#
# How: one MARKER string per patch, searched for in the patched file with a
# plain file(READ)/string(FIND) -- no Python, no git, no stamp, ~microseconds.
# A marker is a symbol the patch itself introduces (tests/scripts/
# test_zephyr_patch_guard.py proves every marker appears in that patch's `+`
# lines, so the table cannot drift from patches.yml).  Chosen over re-running
# scripts/verify_west_patches.py (`git apply --reverse --check`) because a
# marker has no false positive when a workspace legitimately carries a LATER
# edit on top of the patch (that check reports DRIFTED and would block a
# working build), needs no git checkout of the module (tarball/CI-cache
# workspaces), and costs nothing per configure.  The authoritative full verify
# stays in scripts/bootstrap.sh / verify_west_patches.py.
#
# Scope: an entry only fires when the Kconfig symbol(s) that make the build
# depend on the patch are enabled, so a build that never touches the camera is
# never blocked.  Severity is FATAL_ERROR (the missing patch is a guaranteed
# runtime failure of a feature the build enabled).
#
# Opt-out for people who manage patches another way (vendored tree, rebased
# fork): -DALP_SKIP_PATCH_CHECK=ON.
#
# Inputs: ZEPHYR_BASE, ZEPHYR_ALIF_MODULE_DIR, CONFIG_* (set by Zephyr; also
# settable with -D when run as `cmake -P` by the unit test).

if(ALP_SKIP_PATCH_CHECK)
  message(STATUS "alp-sdk patch guard: skipped (ALP_SKIP_PATCH_CHECK=ON)")
  return()
endif()

set(_alp_pg_missing "")

# _alp_pg_check(<patch id> <module> <root dir> <file rel to root> <marker> <Kconfig symbol>...)
# <module> is the `module:` value from zephyr/patches.yml (what
# `west patch --dst-module` takes).  Enabled when ANY listed symbol is set.
function(_alp_pg_check id module root file marker)
  set(_enabled FALSE)
  foreach(_sym IN LISTS ARGN)
    if(${_sym})
      set(_enabled TRUE)
    endif()
  endforeach()
  if(NOT _enabled)
    return()
  endif()
  set(_path "${root}/${file}")
  set(_ok FALSE)
  if(NOT root STREQUAL "" AND EXISTS "${_path}")
    file(READ "${_path}" _content)
    string(FIND "${_content}" "${marker}" _pos)
    if(NOT _pos EQUAL -1)
      set(_ok TRUE)
    endif()
  endif()
  if(NOT _ok)
    set(_alp_pg_missing "${_alp_pg_missing}\n  - ${id} (module '${module}'): '${marker}' not found in ${_path}" PARENT_SCOPE)
    set(_alp_pg_modules "${_alp_pg_modules};${module}" PARENT_SCOPE)
  endif()
endfunction()

# CSI pixel clock: video_csi_dw.c calls clock_control_set_rate() on the CSI pixel
# clock; upstream clock_control_alif.c returns -ENOTSUP without this patch.
_alp_pg_check(zephyr/0001 zephyr "${ZEPHYR_BASE}"
  drivers/clock_control/clock_control_alif.c ALIF_CSI_PIXCLK_CTRL_REG_OFF
  CONFIG_VIDEO_MIPI_CSI2_DW CONFIG_VIDEO_ALIF_CAM)

# IMX335: upstream registers no VIDEO_CID_LINK_FREQ, so the DW CSI-2 host
# programs the wrong D-PHY rate.
_alp_pg_check(zephyr/0004 zephyr "${ZEPHYR_BASE}"
  drivers/video/imx335.c IMX335_LINK_FREQ_HZ
  CONFIG_VIDEO_IMX335)

# ISP IMX335 AE envelope (hal_alif patch 0014 adds this header).
_alp_pg_check(hal_alif/0014 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/imx335_ae_envelope.h IMX335_AE_
  CONFIG_VIDEO_ISP_VSI_CALIB_IMX335)

if(_alp_pg_missing)
  list(REMOVE_DUPLICATES _alp_pg_modules)
  set(_fix "")
  foreach(_m IN LISTS _alp_pg_modules)
    if(NOT _m STREQUAL "")
      set(_fix "${_fix}\n    west patch --dst-module ${_m} apply")
    endif()
  endforeach()
  message(FATAL_ERROR
    "alp-sdk: this Zephyr workspace is missing patches from alp-sdk's "
    "zephyr/patches.yml that the enabled features depend on:${_alp_pg_missing}\n"
    "Without them the build succeeds but fails at runtime (e.g. camera: "
    "'Failed to set CSI pixel clock rate! ret - -134' -> ALP_ERR_NOSUPPORT).\n"
    "Fix (from the workspace root; or run `bash scripts/bootstrap.sh`, which "
    "applies and verifies all patches):${_fix}\n"
    "Then verify with: python3 scripts/verify_west_patches.py\n"
    "Managing patches yourself? Re-run cmake with -DALP_SKIP_PATCH_CHECK=ON.")
endif()
