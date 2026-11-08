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
# Coverage (#2949): every patch in patches.yml whose absence the build does NOT
# notice -- camera/ISP, the AEN MCUboot MRAM do_boot / swap-trailer fixes and the
# RZ TINT status clear.  The mcuboot/sim-only regression-test patches
# (mcuboot/0003, 0005), the ipm poll patch (zephyr/0002), the se-service
# patches (hal_alif/0001, 0002, 0015) and the Kconfig-serialize patch
# (zephyr/0003) fail at build/link time when absent, so they need no guard.
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

# _alp_pg_check(<patch id> <module> <root dir> <file rel to root> <marker> [ALL] <Kconfig symbol>...)
# <module> is the `module:` value from zephyr/patches.yml (what
# `west patch --dst-module` takes).  <marker> is one token, or a quoted string
# when it contains spaces.  Enabled when ANY listed symbol is set, or -- with
# the leading ALL keyword -- only when EVERY listed symbol is set.
function(_alp_pg_check id module root file marker)
  set(_syms ${ARGN})
  set(_all FALSE)
  if(_syms MATCHES "^ALL(;|$)")
    set(_all TRUE)
    list(REMOVE_AT _syms 0)
  endif()
  if(_all)
    set(_enabled TRUE)
  else()
    set(_enabled FALSE)
  endif()
  foreach(_sym IN LISTS _syms)
    if(_all AND NOT ${_sym})
      set(_enabled FALSE)
    elseif(NOT _all AND ${_sym})
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

# IMX335: the first register write after power-up is polled for up to 1 s;
# without it a cold power-up NACKs 0x3000 and the sensor stays NOT_READY.
_alp_pg_check(zephyr/0006 zephyr "${ZEPHYR_BASE}"
  drivers/video/imx335.c IMX335_POWER_UP_TIMEOUT_MS
  CONFIG_VIDEO_IMX335)

# ISP IMX335 AE envelope (hal_alif patch 0014 adds this header).
_alp_pg_check(hal_alif/0014 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/imx335_ae_envelope.h IMX335_AE_
  CONFIG_VIDEO_ISP_VSI_CALIB_IMX335)

# ISP (hal_alif) -- all of these are silent: the library still builds, the image
# just mis-maps the Bayer phase, drops the AE write-back or loads the wrong
# calibration.  Every ISP build carries the generic wrapper patches.
_alp_pg_check(hal_alif/0003 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c "case VIDEO_PIX_FMT_SBGGR10P:"
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0004 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/isp_conf.h "defined(CONFIG_ISP_LIB_WBM_MODULE)"
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0005 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c gain_write_err_logged
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0006 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/sensor_attributes.h CONFIG_VIDEO_ISP_VSI_AE_TARGET
  CONFIG_VIDEO_ISP_VSI_CALIB_OV5647)
_alp_pg_check(hal_alif/0007 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c isp_calib_loaded
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0008 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/isp_param_conf.h "{-881824, -471580, -1441613}"
  CONFIG_VIDEO_ISP_VSI_CALIB_OV5647)
_alp_pg_check(hal_alif/0009 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c "Upstream alifsemi/hal_alif order"
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0010 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c isp_clamp_ctrl_val
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0011 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/ov5647_ae_envelope.h OV5647_AE_
  CONFIG_VIDEO_ISP_VSI_CALIB_OV5647)
_alp_pg_check(hal_alif/0012 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/src/isp_api_wrapper.c "case VIDEO_PIX_FMT_SRGGB10P:"
  CONFIG_VIDEO_ISP_VSI)
_alp_pg_check(hal_alif/0013 alif "${ZEPHYR_ALIF_MODULE_DIR}"
  drivers/isp/isp_wrapper/inc/imx296_ae_envelope.h IMX296_AE_
  CONFIG_VIDEO_ISP_VSI_CALIB_IMX296)

# RZ/V2H/V2N TINT: upstream clears TSCLR (write-1-to-clear) with
# TINT_STATUS_READ(tint) | BIT(tint), dropping the pending status of every slot,
# including the ones the A55 owns under AMP (#2692).
_alp_pg_check(zephyr/0005 zephyr "${ZEPHYR_BASE}"
  drivers/interrupt_controller/intc_renesas_rz_tint.c
  "TINT_STATUS_CLEAR(tint)  REG_TSCLR_WRITE(tint, BIT(tint))"
  CONFIG_RENESAS_RZ_TINT_SUPPORT_STATUS_CLEAR_REG)

# AEN MCUboot (mcuboot module).  Without 0001 do_boot() takes the slot vector
# from CONFIG_FLASH_BASE_ADDRESS (ITCM, 0x0) instead of the MRAM slot -> garbage
# jump -> reboot loop; without 0002/0004 an erased swap_size / unscrambled
# trailer reads as "nothing to do" on the 0x00-erase MRAM.  The guard runs in
# the MCUboot child image (CONFIG_MCUBOOT) and in the application image that
# selects it (CONFIG_BOOTLOADER_MCUBOOT); MRAM_FLASH_ALIF limits it to AEN.
foreach(_alp_pg_mcuboot_sym CONFIG_MCUBOOT CONFIG_BOOTLOADER_MCUBOOT)
  _alp_pg_check(mcuboot/0001 mcuboot "${ZEPHYR_MCUBOOT_MODULE_DIR}"
    boot/zephyr/flash_map_extended.c alif_mram_flash_controller
    ALL ${_alp_pg_mcuboot_sym} CONFIG_MRAM_FLASH_ALIF)
  _alp_pg_check(mcuboot/0002 mcuboot "${ZEPHYR_MCUBOOT_MODULE_DIR}"
    boot/bootutil/src/bootutil_misc.c "is_erased(fap, swap_size,"
    ALL ${_alp_pg_mcuboot_sym} CONFIG_MRAM_FLASH_ALIF)
  _alp_pg_check(mcuboot/0004 mcuboot "${ZEPHYR_MCUBOOT_MODULE_DIR}"
    boot/bootutil/src/bootutil_area.c "(alp-sdk#1066) end_offset"
    ALL ${_alp_pg_mcuboot_sym} CONFIG_MRAM_FLASH_ALIF)
endforeach()

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
    "'Failed to set CSI pixel clock rate! ret - -134' -> ALP_ERR_NOSUPPORT; "
    "AEN MCUboot: garbage jump -> reboot loop).\n"
    "Fix (from the workspace root; or run `bash scripts/bootstrap.sh`, which "
    "applies and verifies all patches):${_fix}\n"
    "Then verify with: python3 scripts/verify_west_patches.py\n"
    "Managing patches yourself? Re-run cmake with -DALP_SKIP_PATCH_CHECK=ON.")
endif()
