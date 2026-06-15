# SPDX-License-Identifier: Apache-2.0
# Zephyr-module entry point for the Alp SDK.
#
# Loaded by Zephyr's build via zephyr/module.yml when CONFIG_ALP_SDK=y.
# Plain-CMake (add_subdirectory) consumers don't reach this file —
# they use the repo root CMakeLists.txt instead.

# Alif Ensemble build-bridge -- applied BEFORE the CONFIG_ALP_SDK early-return
# because it is needed for *any* Ensemble Zephyr build, not just alp-feature
# builds.  Upstream Zephyr v4.4's (minimal) Alif SoC layer omits
# soc/alif/ensemble/common/soc_common.h, but hal_alif v2.2.0 builds
# drivers/dma_event_router/src/dma_event_router.c *unconditionally* for the
# Ensemble family and that TU does `#include <soc_common.h>`.  Supply our
# bridge copy ONLY for Ensemble builds, so it cannot shadow another vendor's
# soc_common.h (e.g. NXP's on i.MX93).  See zephyr/soc-bridge/alif/soc_common.h.
if(CONFIG_SOC_FAMILY_ENSEMBLE OR CONFIG_SOC_FAMILY_ENSEMBLE_RTSS)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/soc-bridge/alif)
endif()

if(NOT CONFIG_ALP_SDK)
    return()
endif()

zephyr_library_named(alp_sdk)

zephyr_library_sources(
    # Runtime capability table -- always compiled; pure .rodata, no Kconfig guard.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/cap.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/delay_zephyr.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/handles.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/last_error.c
    # Camera class dispatcher + stub backend.  The real Zephyr
    # drivers/video/ backend (priority 50) + V2N N44 ISP-aware
    # backend (priority 100 on renesas:rzv2n:n44) ride above the
    # wildcard stub via the opt-in Kconfigs below.  Vendor-ext
    # bodies for the Mali-C55 (NOSUPPORT today) + V2N N44 ISP
    # (ALP_OK skeleton) compile unconditionally so apps that
    # #include the ext headers link cleanly regardless of which
    # silicon they target.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/camera_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/zephyr_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/alif/camera.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/renesas/camera.c
)

# Storage class dispatcher + backends.
# The dispatcher compiles unconditionally so apps that depend on the
# <alp/storage.h> ABI always link cleanly; each backend gates on its
# own Kconfig so builds that don't enable a path don't pay its code
# size.  Vendor-ext bodies under src/backends/ext/{alif,nxp}/storage.c
# compile unconditionally -- they only emit NOSUPPORT + parameter
# validation today, so they pull zero vendor dependencies.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/storage_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/alif/storage.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/nxp/storage.c)

if(CONFIG_ALP_SDK_STORAGE_ZEPHYR_FLASH)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/storage/zephyr_flash.c)
endif()

if(CONFIG_ALP_SDK_STORAGE_ZEPHYR_LITTLEFS)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/storage/zephyr_littlefs.c)
endif()

# Renesas RZ SPI_B (RSPI) controller driver -- carried here (not upstream
# Zephyr) so it survives a `west update`.  Upstream Zephyr ships no driver
# for the SPI_B IP the rzv FSP serves; this one wraps FSP r_spi_b and adds
# the RZ/V2N CM33 INTSEL selectable-interrupt routing.  Needs Zephyr's
# private drivers/spi/spi_context.h on the include path.
if(CONFIG_SPI_RENESAS_RZ_SPI_B)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/spi/spi_renesas_rz_spi_b.c)
    zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/spi)
endif()

# Renesas RZ SCI-B Simple-SPI controller driver + vendored FSP module.
# The on-module GD32 supervisor SPI fast path is wired to SCI7 (P76/P77/P96/P97
# = MOSI7/MISO7/SCK7/SS7), an SCI channel in clock-synchronous Simple-SPI mode,
# NOT the dedicated SPI_B IP.  Unlike SPI_B, the rzv FSP ships no SCI-SPI module,
# so the FSP r_sci_b_spi.c (BSD-3, near-identical to the RA source -- one
# documented one-line deviation -- with an RZ cfg shim) is carried here under
# zephyr/drivers/spi/r_sci_b_spi/ and gated by
# USE_RZ_FSP_SCI_B_SPI.  Both the glue driver and the FSP module need Zephyr's
# private drivers/spi/spi_context.h on the include path.
if(CONFIG_SPI_RENESAS_RZ_SCI_B)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/spi/spi_renesas_rz_sci_b.c)
    zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/spi)
endif()
if(CONFIG_USE_RZ_FSP_SCI_B_SPI)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/spi/r_sci_b_spi/r_sci_b_spi.c)
    zephyr_library_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/spi/r_sci_b_spi)
    # SCI7 DMA fast path: the rzv FSP DMAC-B transfer driver.  hal_renesas
    # ships it for rzv2n (src/rzv/r_dmac_b + the SCI7 RXI/TXI trigger events in
    # bsp/mcu/rzv2n/bsp_dmac.h) but its rzv CMake does not compile it and the
    # rzv2n devicetree has no dmac node, so the glue driver hand-instantiates
    # the transfer_instance_t pair and we build the FSP module here.  Its
    # r_dmac_b_cfg.h shim lives in the r_sci_b_spi include dir above.
    zephyr_library_sources(
        ${ZEPHYR_HAL_RENESAS_MODULE_DIR}/drivers/rz/fsp/src/rzv/r_dmac_b/r_dmac_b.c)
    zephyr_library_include_directories(
        ${ZEPHYR_HAL_RENESAS_MODULE_DIR}/drivers/rz/fsp/inc/instances/rzv)
endif()

# Alif Ensemble ARM MHUv2 inter-core mailbox driver -- vendor-ext,
# BENCH-UNVERIFIED.  Carried here (not upstream Zephyr) so it survives a
# `west update`.  Standard Zephyr MBOX-class driver (DT_DRV_COMPAT
# alif_mhuv2_mbox); doorbell-only, backs alp_rpc_open / OpenAMP static-vrings on
# AEN.  Cannot build under native_sim (Cortex-M55 target), hence
# bench-unverified in CI.  Driver: zephyr/drivers/mbox/mbox_alif_mhuv2.c;
# binding: zephyr/dts/bindings/mbox/alif,mhuv2-mbox.yaml.
zephyr_library_sources_ifdef(CONFIG_MBOX_ALIF_MHUV2
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/mbox/mbox_alif_mhuv2.c)

# Alif Ensemble LPI2C (on-module BRD_I2C bus).  Ported from the Apache-2.0
# zephyr_alif fork; upstream Zephyr + hal_alif ship no LPI2C driver.
zephyr_library_sources_ifdef(CONFIG_I2C_ALIF_LPI2C
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/i2c/i2c_alif_lpi2c.c)

# Alif Ensemble Ethernet (GMAC) platform glue backing upstream eth_dwmac
# (compatible alif,ethernet); driver: zephyr/drivers/ethernet/eth_dwmac_alif_ensemble.c,
# binding: zephyr/dts/bindings/ethernet/alif,ethernet.yaml.  The clock id it
# enables (ALIF_ETHERNET_CLK) is re-encoded for the upstream alif,clockctrl in
# zephyr/include/zephyr/dt-bindings/clock/alif-ensemble-clocks-ext.h.  bench-unverified.
zephyr_library_sources_ifdef(CONFIG_ETH_DWMAC_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/ethernet/eth_dwmac_alif_ensemble.c)
# The glue uses the eth_dwmac core's private header (struct dwmac_priv); upstream
# keeps it co-located in drivers/ethernet/, so expose that dir on the include path.
if(CONFIG_ETH_DWMAC_ALIF)
    zephyr_include_directories(${ZEPHYR_BASE}/drivers/ethernet)
endif()

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/storage/sw_fallback.c)
# USB registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/usb_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/usb/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/usb/sw_fallback.c)
# BLE registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/ble_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ble/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ble/sw_fallback.c)
# Wi-Fi registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/wifi_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wifi/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wifi/sw_fallback.c)
# MQTT registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/mqtt_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/mqtt/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/mqtt/sw_fallback.c)
# mproc registry sources.  The dispatcher owns three handle pools
# (shmem + mbox + hwsem) keyed off the same class registry; the
# portable Zephyr backend covers all three primitives via
# DT_ALIAS(alp_shmemN) / DT_ALIAS(alp_mboxN) / intra-core k_sem
# fallback.  Dispatcher + Zephyr backend ship unconditionally;
# sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/mproc_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/mproc/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/mproc/sw_fallback.c)
# Security registry sources.  The dispatcher owns two handle pools
# (hash + AEAD) plus a cached ops pointer for the stateless
# alp_random_bytes fast path; the portable Zephyr backend uses
# PSA Crypto (PSA picks the active SoC's HW accelerator
# transparently via MbedTLS's driver layer).  Dispatcher + Zephyr
# backend ship unconditionally; sw_fallback gated on the
# native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/security_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_MBEDTLS
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/security/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/security/sw_fallback.c)
# Update-log surface (<alp/update_log.h>) -- experimental, opt-in.
# Pure engine + software tamper-evident tier + (compiled, unregistered)
# Alif TF-M stub seam. Gated on CONFIG_ALP_SDK_UPDATE_LOG.  The dispatch
# owns the public surface + the singleton handle; sw_tier registers the
# universal silicon_ref="*" backend; alif_tfm_stub is compiled (so it
# can't bitrot) but deliberately NOT registered until its silicon lands.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_UPDATE_LOG
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/engine.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/sha256.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/sw_tier.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/alif_tfm_stub.c)
# Audio registry sources.  The dispatcher owns two handle pools
# (alp_audio_in_t + alp_audio_out_t) keyed off the same class
# registry; the portable Zephyr backend wires DMIC input +
# alp_i2s_*-delegated output.  Dispatcher + Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/audio_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/audio/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/audio/sw_fallback.c)
# RPC registry sources.  The dispatcher owns one alp_rpc_channel_t
# pool keyed off the same class registry; the portable Zephyr
# backend carries the ipc_service / RPMsg body + the per-channel
# subscription table + the synchronous-call slot.  Dispatcher +
# Zephyr backend ship unconditionally; sw_fallback gated on the
# native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/rpc_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rpc/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rpc/sw_fallback.c)
# Inference class dispatcher + backends.
# The dispatcher compiles unconditionally so apps that depend on
# the <alp/inference.h> ABI always link cleanly; each backend
# gates on its own Kconfig so builds that don't enable a path
# don't pay its code size.  Vendor-ext bodies under
# src/backends/ext/{renesas,deepx}/inference.c compile
# unconditionally -- they only emit NOSUPPORT + parameter
# validation today, so they pull zero vendor dependencies.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/inference_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/renesas/inference.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/deepx/inference.c)

if(CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/tflm.cpp)
endif()

if(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/ethos_u_aen.cpp)
endif()

if(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/ethos_u_n93.cpp)
endif()

if(CONFIG_ALP_SDK_INFERENCE_BACKEND_DRPAI_V2N)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/drpai_v2n_stub.c)
endif()

if(CONFIG_ALP_SDK_INFERENCE_BACKEND_DEEPX_DXM1)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/deepx_dxm1_stub.c)
endif()

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/sw_fallback.c)
# Camera real backends.  Gate each on its own Kconfig
# opt-in so builds that don't need the Zephyr video subsystem or
# the V2N N44 ISP skeleton don't pay their code size.  The
# wildcard stub stays unconditionally compiled (above) so
# alp_camera_open keeps succeeding on every supported SoC.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/zephyr_video.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/v2n_n44_isp.c)

# v0.2 peripheral wrappers — opt-in per-class so apps that don't use
# a given Zephyr subsystem don't link the wrapper.  Each defaults on
# when its underlying Zephyr driver class is enabled.
# DAC class dispatcher.  Owns the alp_dac_* API surface
# (alp_dac_open / write_mv / read_mv / close) and routes through the
# backend registry.  Backends register under .alp_backends_dac and
# ship from src/backends/dac/*.c.  The dispatcher compiles
# unconditionally (mirrors rtc_dispatch); the zephyr_drv backend
# guards its DT-alias body on CONFIG_DAC internally.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/dac_dispatch.c)
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/dac/zephyr_drv.c)

if(CONFIG_ALP_SDK_V2N_SUPERVISOR)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/dac/gd32_bridge.c)
endif()

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/dac/sw_fallback.c)
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/rtc_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_RTC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rtc/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rtc/sw_fallback.c)
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/wdt_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_WATCHDOG
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wdt/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wdt/sw_fallback.c)
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/counter_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/zephyr_drv.c)

if(CONFIG_ALP_SDK_V2N_SUPERVISOR)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/gd32_bridge.c)
endif()

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/sw_fallback.c)
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/qenc_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_GPIO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/qenc/zephyr_drv.c)

if(CONFIG_ALP_SDK_V2N_SUPERVISOR)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/qenc/gd32_bridge.c)
endif()

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/qenc/sw_fallback.c)
# TMU registry sources.  TMU is handle-less; the dispatcher caches
# the selected backend's ops vtable on first call instead of owning
# a handle pool.  Dispatcher + portable Zephyr/bridge backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_TMU
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/tmu_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/tmu/zephyr_drv.c)

if(CONFIG_ALP_SDK_PERIPH_TMU AND CONFIG_ALP_SDK_TMU_SW_FALLBACK)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/tmu/sw_fallback.c)
endif()

# DSP registry sources.  The dispatcher owns one alp_dsp_chain_t
# pool keyed off the dsp class registry; the OS-agnostic CMSIS-DSP /
# portable-radix-2 body lives in src/backends/dsp/sw_fallback.c (no
# separate zephyr_drv exists -- the V2N GD32G5 HW-FFT surfaces via
# wave-2's alp_adc_filter_t / spectrum_t in <alp/adc.h>, not
# through this standalone class, so the SW backend is the
# unconditional production pick today).
#
# Dispatcher + sw_fallback ship unconditionally -- they ARE the
# production code on every SoC.  CONFIG_ALP_SDK_DSP is the opt-in
# feature gate; when off, neither file is linked so the static
# cost (~17 KB pool slot) is paid only by apps that use the chain
# API.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_DSP
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/dsp_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/dsp/sw_fallback.c)

# System-power-mode surface (<alp/power.h>).  Wired behind
# CONFIG_ALP_SDK_POWER so customers can opt out on builds that
# don't need sleep transitions.  Class dispatcher + wildcard stub
# backend ship unconditionally; the real Zephyr pm_policy_*
# backend rides above the stub at priority 100 on builds that
# link Zephyr's PM subsystem (gated on CONFIG_PM so the
# pm_policy_state_lock_* symbols are visible).  V2N supervisor
# CMD_POWER_MODE_SET routes through the Renesas vendor-ext body
# whenever CONFIG_ALP_SDK_V2N_SUPERVISOR=y.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_POWER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/power_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/power/zephyr_stub.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_POWER_PM_POLICY
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/power/zephyr_pm_policy.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_POWER_EXT_RENESAS
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ext/renesas/power.c)

# Display class dispatcher + stub backend.  Real backends (Zephyr
# display_* driver-class wrapper for SSD1306 / ILI9341 / ST7789 on
# AEN, V2N DSI / parallel-RGB framebuffer paths) land per issue #23.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/display_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/display/zephyr_stub.c)

# 2D graphics accelerator surface (<alp/gpu2d.h>).  The class
# dispatcher ships whenever the surface is enabled; the portable
# software fallback (sw_fallback.c -- the priority-0 wildcard that
# does real CPU fill/blit/blend) is gated on its own opt-in so it
# always builds on native_sim.  The Alif D/AVE 2D real backend is
# gated on the proprietary Dave2D pack being on the include path
# (BENCH-UNVERIFIED, never built on native_sim).  No NXP/Vivante
# backend exists: i.MX 93's 2D engine is PXP, not Vivante -- see
# docs/adr/0008-gpu2d-portable-shim.md.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPU2D
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/gpu2d_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPU2D_SW_FALLBACK
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpu2d/sw_fallback.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpu2d/alif_dave2d.c)

# Runtime hardware-info reader (EEPROM manifest + BOARD_ID ADC).
# v0.3 stub returns NOSUPPORT; v0.3.x fills in the real impl.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_HW_INFO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/hw_info_zephyr.c)

# Chip drivers — gated on per-chip Kconfig so blocks that don't use a
# given chip don't pay its code size.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LSM6DSO
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lsm6dso/lsm6dso.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1306
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1306/ssd1306.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BLOCK_BUTTON_LED
    ${ZEPHYR_CURRENT_MODULE_DIR}/blocks/button_led/button_led.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BME280
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bme280/bme280.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LIS2DW12
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lis2dw12/lis2dw12.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1331
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1331/ssd1331.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV5640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov5640/ov5640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BLOCK_PDM_MIC
    ${ZEPHYR_CURRENT_MODULE_DIR}/blocks/pdm_mic/pdm_mic.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ICM42670
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/icm42670/icm42670.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMI323
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmi323/bmi323.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMP581
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmp581/bmp581.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CC3501E
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/cc3501e/cc3501e.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TMP112
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tmp112/tmp112.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RV3028C7
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/rv3028c7/rv3028c7.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/optiga_trust_m/optiga_trust_m.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_EEPROM_24C128
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/eeprom_24c128/eeprom_24c128.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CAM_MUX_PI3WVR626
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/cam_mux_pi3wvr626/cam_mux_pi3wvr626.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TCAL9538
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tcal9538/tcal9538.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TAS2563
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tas2563/tas2563.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_INA236
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ina236/ina236.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ACT8760
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/act8760/act8760.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DA9292
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/da9292/da9292.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TPS628640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tps628640/tps628640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_PI3DBS12212
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/pi3dbs12212/pi3dbs12212.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GD32G553
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gd32g553/gd32g553.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RTL8211FDI
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/rtl8211fdi/rtl8211fdi.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CLK_5L35023B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/clk_5l35023b/clk_5l35023b.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MURATA_LBEE5HY2FY
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/murata_lbee5hy2fy/murata_lbee5hy2fy.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DEEPX_DXM1
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/deepx_dxm1/deepx_dxm1.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GD32_SWD
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gd32_swd/gd32_swd.c)

# v0.5 §D.AI batch -- 18 vision / display / accelerator chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV2640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov2640/ov2640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV5645
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov5645/ov5645.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV7670
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov7670/ov7670.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV9281
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov9281/ov9281.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_AR0234
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ar0234/ar0234.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IMX219
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/imx219/imx219.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IMX477
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/imx477/imx477.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GC2145
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gc2145/gc2145.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TI_DS90UB953_954
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ti_ds90ub953_954/ti_ds90ub953_954.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAXIM_MAX9295_9296
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/maxim_max9295_9296/maxim_max9295_9296.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ST7789
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/st7789/st7789.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ILI9341
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ili9341/ili9341.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ILI9488
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ili9488/ili9488.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RA8875
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ra8875/ra8875.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SH1106
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/sh1106/sh1106.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IL3820
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/il3820/il3820.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GDEW0154T8
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gdew0154t8/gdew0154t8.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_HAILO_8L
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/hailo_8l/hailo_8l.c)

# v0.5 §D.industrial batch -- 18 industrial sensing / control chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMP390
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmp390/bmp390.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MS5611
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ms5611/ms5611.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LPS22HB
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lps22hb/lps22hb.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VL53L1X
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/vl53l1x/vl53l1x.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VL53L5CX
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/vl53l5cx/vl53l5cx.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_A02YYUW
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/a02yyuw/a02yyuw.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DRV8833
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/drv8833/drv8833.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DRV8825
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/drv8825/drv8825.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TMC2209
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tmc2209/tmc2209.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_A4988
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/a4988/a4988.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_AS5048A_B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/as5048a_b/as5048a_b.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MT6701
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/mt6701/mt6701.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_HX711
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/hx711/hx711.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX31855
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max31855/max31855.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX31865
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max31865/max31865.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TSL2591
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tsl2591/tsl2591.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QMC5883L
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/qmc5883l/qmc5883l.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VEML7700
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/veml7700/veml7700.c)

# v0.5 §D.iot batch -- 9 IoT / connectivity chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QUECTEL_BG95
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/quectel_bg95/quectel_bg95.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QUECTEL_BG77
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/quectel_bg77/quectel_bg77.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_SARA_R5
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_sara_r5/ublox_sara_r5.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SEMTECH_SX1262
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/semtech_sx1262/semtech_sx1262.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SEMTECH_SX1276
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/semtech_sx1276/semtech_sx1276.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_NEO_M9N
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_neo_m9n/ublox_neo_m9n.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_MAX_M10S
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_max_m10s/ublox_max_m10s.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ATGM336H
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/atgm336h/atgm336h.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ATECC608B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/atecc608b/atecc608b.c)

# v0.5 §D.audio batch -- 6 audio chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ICS_43434
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ics_43434/ics_43434.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_INMP441
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/inmp441/inmp441.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_WM8960
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/wm8960/wm8960.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TLV320AIC3204
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tlv320aic3204/tlv320aic3204.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX98357A
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max98357a/max98357a.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ES8388
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/es8388/es8388.c)

# V2N supervisor singleton -- the lazy-init GD32G553 ctx the portable
# PWM/ADC/DAC/QENC/COUNTER backends dispatch through on V2N.  The
# .c file emits NOSUPPORT stubs when CONFIG_ALP_SDK_V2N_SUPERVISOR=n
# so the backends compile unconditionally.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/v2n_supervisor.c
)

# V2N DEEPX rail-mgmt auxiliary.  Driven by P65 rising-edge IRQ;
# brings up DA9292 CH2 = 0.75 V via BRD_I²C + drives P64 high to
# release the rail.  Compiles to a NOSUPPORT stub when either the
# supervisor or the power-mgmt feature is disabled.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/v2n_power_mgmt.c
)

# LwRB: publish the vendor stub headers on the include path so SDK
# code compiles against <lwrb/lwrb.h> regardless of whether the
# upstream MaJerle/lwrb module is on the workspace.  When the
# `extras-v04` west group is enabled the upstream sources end up at
# modules/lib/lwrb/src/include/ and (once registered as a Zephyr
# module via EXTRA_ZEPHYR_MODULES) win the include search ahead of
# our stub; the ABI is identical so the SDK source is unchanged
# across the swap.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/lwrb/include)

# Compile our in-tree LwRB stub impl whenever the upstream module is
# *not* present on the workspace.  ZEPHYR_LWRB_MODULE_DIR is set by
# Zephyr's module-resolution pass when modules/lib/lwrb is picked up;
# absent that, we provide the lwrb_write/read/peek/skip/advance/get_*
# symbols ourselves.
if(NOT DEFINED ZEPHYR_LWRB_MODULE_DIR)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/lwrb/src/lwrb_stub_impl.c
    )
endif()

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
# src/common holds the internal mproc framing helper that tests
# include directly to cover encode/decode without a real mbox peer.
# Header path is `proto/alp_mproc_frame.h` so apps that don't opt in
# never see new symbols pollute the include namespace.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/src/common)
zephyr_library_include_directories(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference
)

# nanopb placeholder framing for <alp/mproc.h> (v0.4 prep).  Always
# compiled so the failure-path tests link; only invoked at runtime
# when CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y.  Replaced by the
# nanopb-generated codec when the `extras-v04` west group lands.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/proto/alp_mproc_frame.c
)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/nanopb/include)

# Instance-capability helpers (<alp/cap_instance.h>).  Hand-written
# runtime logic that does not depend on per-SoC metadata; always
# compiled so any subsystem can call alp_capabilities_has().
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/cap_helpers.c
)

# .alpmodel package reader (<alp/model.h>).  OS-agnostic read-side
# parser (24-byte header + zcbor manifest decode + blob table).
# Gated on CONFIG_ALP_SDK_MODEL_READER, which selects ZCBOR.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_MODEL_READER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/alp_model.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/alp_model_loader.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/inference/alp_model_select.c
)

# Backend registry selector (<alp/backend.h>).  Walks the per-class
# linker sections emitted by ALP_BACKEND_REGISTER + ALP_BACKEND_DEFINE_CLASS
# to find the best-matching backend for the active silicon_ref.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backend.c
)

# I2C class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_I2C_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/i2c_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_I2C
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2c/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2c/sw_fallback.c)
# SPI class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_SPI_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/spi_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_SPI
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/spi/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/spi/sw_fallback.c)
# UART class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_UART_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/uart_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/uart/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/uart/sw_fallback.c)
# GPIO class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_GPIO_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/gpio_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_GPIO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpio/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpio/sw_fallback.c)
# PWM class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_PWM_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/pwm_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_PWM
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/pwm/zephyr_drv.c)
# V2N drives zero native E1M PWMs -- all eight route through the
# GD32G553 supervisor MCU.  SoC-gated like the adc bridge so it links
# only on the V2N target (where the supervisor symbols exist).
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_RENESAS_RZV2N_N44
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/pwm/gd32_bridge.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/pwm/sw_fallback.c)
# I2S class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_I2S_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/i2s_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_I2S
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2s/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2s/sw_fallback.c)
# CAN class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_CAN_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/can_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_CAN
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/can/zephyr_drv.c)

zephyr_library_sources(        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/can/sw_fallback.c)
# ADC class dispatcher.  Owns the one-shot alp_adc_* API surface
# (alp_adc_open / read_raw / read_uv / close / capabilities) and
# routes through the backend registry.  Backends register under
# .alp_backends_adc and ship from src/backends/adc/*.c.
#
# Streaming (alp_adc_stream_*) lives in src/zephyr/peripheral_adc.c;
# both files coexist under the same Kconfig because they implement
# different API surfaces and share no symbols.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_ADC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/adc_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_adc.c)

# ADC backends.  Each registers itself into .alp_backends_adc via
# ALP_BACKEND_REGISTER.  SoC-gated so unused vendor code is
# tree-shaken from the build.
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/alif_e7.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_RENESAS_RZV2N_N44
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/gd32_bridge.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_ADC_SW_FALLBACK
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/sw_fallback.c)
