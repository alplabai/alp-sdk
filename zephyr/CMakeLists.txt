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

# Alif Ensemble on-die MRAM flash driver (compatible
# "alif,mram-flash-controller", base 0x80000000), vendored VERBATIM from the
# Apache-2.0 zephyr_alif fork.  Compiled into the DEFAULT Zephyr library via
# zephyr_sources_ifdef -- and DELIBERATELY BEFORE the CONFIG_ALP_SDK
# early-return below (the soc-bridge precedent) -- because the MCUboot
# bootloader image needs it but never sets CONFIG_ALP_SDK.  Upstream Zephyr
# v4.4 + hal_alif ship no flash-class driver for the MRAM (it is exposed only
# as a memory region), so MCUboot's flash_map has no driver to read/write
# slot0/slot1; this fork copy sets FLASH_HAS_DRIVER_ENABLED.  Pure CMSIS
# register/memory access: no private upstream header and no hal_alif library,
# so no extra include path.  Binding under zephyr/dts/bindings/flash_controller/;
# node in the board overlay.  ADR 0017 Tier-2 (INTERIM).
zephyr_sources_ifdef(CONFIG_MRAM_FLASH_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/flash/flash_mram_alif.c)

# Alif peripheral-flow PL330 DMA + its Event-Router mux.  Upstream Zephyr's
# dma_pl330 is MEMORY_TO_MEMORY only; this is the sdk-alif fork's peripheral-flow
# PL330 (MEMORY_TO/FROM_PERIPHERAL) + the evtrtr multiplexer, vendored with a
# DISTINCT compatible (alif,dma-pl330 / alif,dma-event-router) + config
# (DMA_PL330_ALIF) so it coexists with upstream's arm,dma-pl330 without symbol
# or devicetree collision.  Enables host-side DMA for continuous SPI streams
# (the CC3501E bridge).  Headers live under zephyr/include/ so the <zephyr/...>
# includes resolve.  ADR 0017 Tier-2 (fork-driver copy; upstream ships no
# peripheral-flow PL330) -- INTERIM, retires onto the opt-in sdk-alif fork.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma)
if(DEFINED ZEPHYR_ALIF_MODULE_DIR)
  zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
endif()
zephyr_sources_ifdef(CONFIG_DMA_PL330_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma/dma_pl330_alif.c)
zephyr_sources_ifdef(CONFIG_DMA_ALIF_EVENT_ROUTER
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma/dma_alif_event_router.c)
# Tier-1.5 glue: the M55-HE PL330 DMA2 boot-manager security latch the fork does
# in its soc_init (self-gates on the alif,dma-pl330 node being present).
zephyr_sources_ifdef(CONFIG_DMA_PL330_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma/dma_alif_boot_latch.c)

if(NOT CONFIG_ALP_SDK)
    return()
endif()

zephyr_library_named(alp_sdk)

zephyr_library_sources(
    # Runtime capability table -- always compiled; pure .rodata, no Kconfig guard.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/cap.c
    # alp_status_name() / alp_status_description() -- generated from the
    # alp_status_t enum (scripts/gen_status_strings.py).  The name table is
    # always compiled (pure .rodata); the description table gates on
    # CONFIG_ALP_STATUS_DESCRIPTIONS internally so this TU compiles either way.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/status_strings.c
    # SDK lifecycle (alp_init / alp_deinit) -- OS-agnostic, always compiled.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/init.c
    # Runtime SDK-version accessor -- always compiled; pure .rodata.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/version.c
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

# Boot-identity banner (CONFIG_ALP_SDK_BANNER, default y) -- one line at
# APPLICATION init naming the SDK + SoM + manufacturer.  Data-driven from the
# SoM identity EEPROM when CONFIG_ALP_SDK_HW_INFO is on (src/zephyr/alp_banner.c).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BANNER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/alp_banner.c)

# Alp SoM console (CONFIG_ALP_SDK_CONSOLE) -- the `alp` Zephyr-shell command
# tree.  Each group is its own TU attaching to the `alp` root via the
# decentralized SHELL_SUBCMD_ADD section mechanism, so they list here flat.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_mem.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_gpio.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_i2c.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_periph.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_sys.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/console/alp_console_companion.c)




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
    # zephyr_littlefs.c includes <zephyr/fs/littlefs.h>, which itself
    # includes <lfs.h> from the upstream littlefs module -- but that
    # header lives outside ZEPHYR_BASE, so the compiler can't find it
    # without this module's include dir on the path. Zephyr's own
    # subsys/fs/littlefs_fs.c gets this for free via the `LITTLEFS`
    # INTERFACE target (zephyr/modules/littlefs/CMakeLists.txt); our
    # alp-sdk zephyr module is a separate CMake library, so it needs
    # its own copy of the include path. ZEPHYR_LITTLEFS_MODULE_DIR is
    # set by Zephyr's module-resolution pass whenever modules/fs/littlefs
    # is present (default-enabled `fs` group in Zephyr's own west.yml,
    # so both an alp-sdk-topdir workspace and CI's `west init -m zephyr`
    # workspace pick it up) -- guard on it being defined rather than
    # assuming presence.
    if(DEFINED ZEPHYR_LITTLEFS_MODULE_DIR)
        zephyr_library_include_directories(${ZEPHYR_LITTLEFS_MODULE_DIR})
    endif()
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

# Alif Ensemble hardware-semaphore (HWSEM) driver -- vendor-ext, BENCH-UNVERIFIED.
# AMP request/release latch shared between the two RTSS cores (M55-HE <-> M55-HP).
# HWSEM has no upstream Zephyr device class, so this is a "misc" (no-class) driver
# exposing a tiny take/give API (DT_DRV_COMPAT alif_hwsem).  Cannot build under
# native_sim (Cortex-M55 target), hence bench-unverified in CI.  Driver:
# zephyr/drivers/misc/alif_hwsem/hwsem_alif.c; binding:
# zephyr/dts/bindings/misc/alif,hwsem.yaml.  Its own include dir is exposed so an
# app (the regcheck example) can include "hwsem_alif.h" for the take/give API.
zephyr_library_sources_ifdef(CONFIG_HWSEM_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/misc/alif_hwsem/hwsem_alif.c)
if(CONFIG_HWSEM_ALIF)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/misc/alif_hwsem)
endif()

# (Retired per ADR 0017) No Alif LPI2C driver: LPI2C0 is slave-only
# (TRM §3.17.4), so a master driver is a dead end; the housekeeping trio is
# Tier-3 (Alif SE services), edge I2C is Tier-1 upstream i2c_dw.

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

# Alif Ensemble DWC_ssi SPI (compatible alif,dwc-ssi-spi), vendored from the
# Apache-2.0 zephyr_alif fork; distinct compatible so it doesn't collide with
# upstream spi_dw.  The driver reuses upstream's private spi_context.h, so put
# drivers/spi on the include path (the SCI-B/eth precedent).
zephyr_library_sources_ifdef(CONFIG_SPI_DW_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/spi/spi_dw_alif.c)
if(CONFIG_SPI_DW_ALIF)
    zephyr_include_directories(${ZEPHYR_BASE}/drivers/spi)
endif()

# Arm MHUv2 mailbox IPM driver (compatible "arm,mhuv2"), vendored from the
# Apache-2.0 zephyr_alif fork (drivers/ipm/ipm_arm_mhuv2.c + ipm_arm_mhuv2.h).
# Upstream Zephyr v4.4 ships only the MHUv1 ipm_mhu.c.  This is the v2 mailbox
# the Ensemble Secure-Enclave service channel rides on: the Apache-2.0 hal_alif
# SE client (se_services/zephyr/src/se_service.c, built by hal_alif itself under
# CONFIG_HAS_ALIF_SE_SERVICES) drives it through Zephyr's IPM API.  Self-contained
# (its register header sits beside it), so put drivers/ipm on the include path
# for the driver's own "ipm_arm_mhuv2.h".  Nodes in the E8 SoC overlay
# (seservice0r/seservice0s + the se_service root node).  ADR 0017 Tier-2.
zephyr_library_sources_ifdef(CONFIG_ARM_MHUV2
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/ipm/ipm_arm_mhuv2.c)
if(CONFIG_ARM_MHUV2)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/ipm)
endif()

# hal_alif's SE client (se_service.c, built by the hal_alif module itself under
# CONFIG_HAS_ALIF_SE_SERVICES) does `#include <soc_memory_map.h>` for
# local_to_global() (the SE message buffer's CPU->global address translation).
# hal_alif only puts its common/include dir on the path under CONFIG_RTSS_HE/HP
# (common/CMakeLists.txt) -- which holds here, but add it explicitly so the SE
# client resolves the header on any Ensemble RTSS build (the video-driver
# precedent above).
if(CONFIG_HAS_ALIF_SE_SERVICES)
    zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
endif()

# Alif Ensemble UTIMER PWM + counter class drivers (compatibles alif,pwm /
# alif,utimer-counter), over the hal_alif drivers/utimer register library
# (the drivers select USE_ALIF_HAL_UTIMER).  Bindings under
# zephyr/dts/bindings/{pwm,counter,timer}/; nodes in the E8 SoC overlay.
zephyr_library_sources_ifdef(CONFIG_PWM_ALIF_UTIMER
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/pwm/pwm_alif_utimer.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER_ALIF_UTIMER
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/counter/counter_alif_utimer.c)

# Synopsys DesignWare APB RTC counter (compatible "snps,dw-apb-rtc"), the E8
# always-on LPRTC at lprtc@42000000.  A free-running 32-bit up-counter with one
# compare/alarm channel -- a DIFFERENT IP from upstream's counter_dw_timer.c
# (snps,dw-timers = DW APB Timers).  Driver + register header vendored Tier-2
# from the Apache-2.0 zephyr_alif fork; binding under
# zephyr/dts/bindings/counter/; node in the E8 SoC overlay.
zephyr_library_sources_ifdef(CONFIG_COUNTER_RTC_SNPS_DW
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/counter/counter_dw_rtc.c)

# Alif Ensemble LPTIMER counter (compatible "alif,lptimer"), the always-on
# lptimer@42001000 block: four independent 32-bit DOWN-counters clocked from the
# VBAT-domain LF source.  A DISTINCT always-on IP from the LPRTC (snps,dw-apb-rtc
# above) and the UTIMER counter.  Clean-room Tier-1.5 driver written directly
# over the memory-mapped registers (the zephyr_alif fork ships no LPTIMER driver
# or "alif,lptimer" binding); binding under zephyr/dts/bindings/counter/; node in
# the E8 SoC overlay.
zephyr_library_sources_ifdef(CONFIG_COUNTER_ALIF_LPTIMER
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/counter/counter_alif_lptimer.c)

# Alif Ensemble ADC class driver (compatible "alif,adc"), vendored VERBATIM from
# the Apache-2.0 zephyr_alif fork; hal_alif ships no ADC class driver, only the
# analog_ctrl VREF helper library (drivers/analog), so the driver selects
# USE_ALIF_HAL_ANALOG to pull that in.  The driver reuses upstream's private
# adc_context.h, so put drivers/adc on the include path (the spi_context.h
# precedent above).  Bindings under zephyr/dts/bindings/adc/; nodes in the E8
# SoC overlay.  ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_ADC_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/adc/adc_alif.c)
if(CONFIG_ADC_ALIF)
    zephyr_include_directories(${ZEPHYR_BASE}/drivers/adc)
endif()

# Alif Ensemble DAC12 class driver (compatible "alif,dac"), vendored VERBATIM
# from the Apache-2.0 zephyr_alif fork; hal_alif ships no DAC class driver, only
# the analog_ctrl VREF helper library (drivers/analog), so the driver selects
# USE_ALIF_HAL_ANALOG to pull that in (shared with ADC_ALIF).  Unlike the ADC, the
# DAC driver pulls in no private upstream context header, so no extra include dir
# is needed.  Bindings under zephyr/dts/bindings/dac/; nodes in the E8 SoC
# overlay.  ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_DAC_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dac/dac_alif.c)

# Alif Ensemble CRC engine driver (compatible "alif,crc"), a CLEAN-ROOM in-tree
# driver implementing the UPSTREAM Zephyr v4.4 CRC class API
# (<zephyr/drivers/crc.h>: crc_begin/update/finish over struct crc_ctx).  NOT
# vendored: the Apache-2.0 zephyr_alif fork's CRC driver targets a fork-private
# API (crc_compute()/struct crc_params, CONFIG_CRC_DRV) that does not exist
# upstream, so it cannot be consumed verbatim -- the register model is instead
# re-authored value-only from the Alif DFP (drivers/include/crc.h + soc.h).
# Polled, memory-mapped block: no hal_alif library, no private upstream context
# header, so no extra include dir is needed.  Binding
# zephyr/dts/bindings/crc/alif,crc.yaml; nodes in the E8 SoC overlay.
# ADR 0017 Tier-1.5 (in-tree-thin over the silicon).
zephyr_library_sources_ifdef(CONFIG_CRC_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/crc/crc_alif.c)

# Alif Ensemble analog comparator class driver (compatible "alif,cmp"), a
# clean-room driver (NOT a vendored fork copy: hal_alif + the available fork
# checkout ship no comparator class driver `.c`, only the binding/nodes/sample).
# Implements the upstream Zephyr v4.4 comparator API over the hal_alif analog_ctrl
# VREF/clock helper library, so the driver selects USE_ALIF_HAL_ANALOG to pull
# that in (shared with ADC_ALIF / DAC_ALIF).  No private upstream context header,
# so no extra include dir is needed.  Binding under
# zephyr/dts/bindings/comparator/; nodes in the E8 SoC overlay.  ADR 0017 Tier-2
# (INTERIM).
zephyr_library_sources_ifdef(CONFIG_CMP_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/comparator/comparator_alif.c)

# Alif Ensemble LPPDM / PDM mic-capture class driver (compatible "alif,alif-pdm"),
# vendored VERBATIM from the Apache-2.0 zephyr_alif fork (drivers/audio/alif_pdm.c).
# Upstream Zephyr v4.4 + hal_alif ship no Alif PDM driver.  Pure CMSIS + Zephyr
# clock_control/pinctrl (no hal_alif library, like flash_mram_alif); implements the
# Zephyr DMIC API.  Binding zephyr/dts/bindings/audio/alif,alif-pdm.yaml + the
# private header zephyr/include/zephyr/drivers/pdm/pdm_alif.h; node in the E8 SoC
# overlay.  ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_ALIF_PDM
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/audio/alif_pdm.c)
if(CONFIG_ALIF_PDM)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma)
if(DEFINED ZEPHYR_ALIF_MODULE_DIR)
  zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
endif()
endif()

# Alif Ensemble DesignWare I2S controller (compatible "snps,designware-i2s"),
# vendored VERBATIM from the Apache-2.0 zephyr_alif fork (drivers/i2s/i2s_dw.c)
# + its private header i2s_dw.h.  Upstream Zephyr v4.4 + hal_alif ship no
# DesignWare I2S driver.  Implements the standard Zephyr I2S API; FIFO/interrupt
# driven (no DMA subsystem dependency -- see CONFIG_I2S_DW).  Binding
# zephyr/dts/bindings/i2s/snps,designware-i2s.yaml; node in the app overlay.
# ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_I2S_DW
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/i2s/i2s_dw.c)
if(CONFIG_I2S_DW)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/i2s)
endif()

# Alif Ensemble UTIMER quadrature decoder (compatible "alif,utimer-qdec"),
# vendored VERBATIM from the Apache-2.0 zephyr_alif fork
# (drivers/sensor/qdec_alif/qdec_alif_utimer.c).  Upstream Zephyr v4.4 + hal_alif
# ship no Alif QDEC driver.  Sensor-class (SENSOR_CHAN_ROTATION), built on the
# hal_alif utimer helpers (USE_ALIF_HAL_UTIMER); binds the "alif,utimer-qdec"
# child + reaches the "alif,utimer" parent via DT_INST_PARENT.  Binding
# zephyr/dts/bindings/sensor/alif,utimer-qdec.yaml.  ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_QDEC_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c)

# Alif Ensemble DesignWare SD Host Controller (compatible "snps,dwc-sdhc"),
# vendored VERBATIM from the Apache-2.0 zephyr_alif fork (drivers/sdhc/sdhc_dwc.c
# + its private header sdhc_dwc.h).  Upstream Zephyr v4.4 + hal_alif ship no
# DesignWare SDHC driver.  Standard Zephyr SDHC API; the card's internal
# ADMA2/SDMA engine moves data (no Alif system-DMA dependency).  The optional
# soc_memory_map.h include is gated behind CONFIG_SDHC_DWC_DMA_ADDR_TRANSLATE
# (off), so identity addressing is used and the itcm/dtcm global_base dtsi gap is
# avoided.  Binding zephyr/dts/bindings/sdhc/snps,dwc-sdhc.yaml.  ADR 0017 Tier-2.
zephyr_library_sources_ifdef(CONFIG_SDHC_DWC
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/sdhc/sdhc_dwc.c)
if(CONFIG_SDHC_DWC)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/sdhc)
endif()

# Alif Ensemble CAMERA CAPTURE path -- CPI controller (compatible "alif,cam"),
# DesignWare MIPI CSI-2 bridge (compatible "snps,designware-csi"), DesignWare
# MIPI D-PHY (compatible "snps,designware-dphy") + ON Semi ARX3A0 sensor
# (compatible "onnn,arx3a0"), all vendored from the Apache-2.0 zephyr_alif fork.
# Upstream Zephyr v4.4 + hal_alif ship NONE of these.  The sources, bindings
# (zephyr/dts/bindings/{video,mipi-dphy}/) and DT nodes (E8 SoC overlay) are
# carried in-tree so they survive a `west update`; the TUs build in directly
# when their DT node is enabled (the former ALP_VIDEO_ALIF_BROKEN gate is
# RETIRED -- the video drivers are now PORTED to the v4.4 video API by Alp Lab
# AB; see the per-driver provenance headers).  The video drivers reuse
# upstream's private drivers/video/video_common.c helpers + the public CSI
# tables vendored to zephyr/include/zephyr/drivers/video/video_alif.h;
# soc_memory_map.h resolves via the hal_alif common/include dir
# (CONFIG_RTSS_HE/HP).  Put the vendored public include dir on the path so
# <zephyr/drivers/video/video_alif.h> and <zephyr/drivers/mipi_dphy/dphy_dw.h>
# resolve.  ADR 0017 Tier-2 (INTERIM).  The ISP (isp_pico.c, compatible
# "vsi,isp-pico") rides on the same v4.4 video API + the hal_alif libisp wrapper;
# it is Tier-2 opt-in (CONFIG_ALP_SDK_CAMERA_ALIF_ISP, default n).
if(CONFIG_VIDEO_ALIF_CAM OR CONFIG_VIDEO_MIPI_CSI2_DW OR CONFIG_VIDEO_ARX3A0 OR CONFIG_MIPI_DPHY_DW OR CONFIG_VIDEO_ISP_VSI)
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma)
if(DEFINED ZEPHYR_ALIF_MODULE_DIR)
  zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
endif()
    zephyr_include_directories(${ZEPHYR_BASE}/drivers/video)
    # video_alif.c / isp_pico.c use hal_alif's <soc_memory_map.h> (local_to_global
    # for the CPI/ISP AXI buffer addresses).  hal_alif only puts its common/include
    # dir on the path under CONFIG_RTSS_HE/HP (common/CMakeLists.txt); add it here
    # explicitly so the CPI/ISP TUs resolve the header on any Ensemble RTSS build.
    if(CONFIG_VIDEO_ALIF_CAM OR CONFIG_VIDEO_ISP_VSI)
        zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
    endif()
endif()
zephyr_library_sources_ifdef(CONFIG_VIDEO_ALIF_CAM
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/video/video_alif.c)
zephyr_library_sources_ifdef(CONFIG_VIDEO_MIPI_CSI2_DW
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/video/video_csi_dw.c)
zephyr_library_sources_ifdef(CONFIG_VIDEO_ARX3A0
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/video/arx3a0.c)
zephyr_library_sources_ifdef(CONFIG_MIPI_DPHY_DW
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/mipi_dphy/dphy_dw.c)
# Alif Ensemble ISP (Pico, compatible "vsi,isp-pico"), vendored from the
# Apache-2.0 zephyr_alif fork + PORTED to the v4.4 video API by Alp Lab AB.  It
# is a video m2m device that links the hal_alif libisp wrapper (the Vivante
# blob, opt-in via USE_ALIF_ISP_LIB).  ADR 0017 Tier-2, opt-in (default n).
zephyr_library_sources_ifdef(CONFIG_VIDEO_ISP_VSI
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/video/isp_pico.c)

# Alif Ensemble CAN-FD controller (CAST CAN-CTRL IP, compatible "cast,can",
# CANFD0 base 0x49036000), vendored VERBATIM from the Apache-2.0 zephyr_alif
# fork (drivers/can/can_cast.c + can_cast.h).  Upstream Zephyr v4.4 ships only
# can_mcan.c (Bosch M_CAN), whose register map does NOT match the CAST IP, and
# hal_alif ships no CAN class driver -- so this fork copy is the only Zephyr
# can_* device for the part.  The alp-sdk portable <alp/can.h> backend
# (src/backends/can/zephyr_drv.c) binds it through the alp_can0 alias.  The
# driver is self-contained CMSIS-style register access via DEVICE_MMIO + the
# Zephyr clock_control / pinctrl / can.h class APIs; it needs only the
# soc_common.h supplied by our Ensemble soc-bridge above (no hal_alif lib, no
# extra include path).  Binding under zephyr/dts/bindings/can/cast,can.yaml;
# node in the E8 SoC overlay.  ADR 0017 Tier-2 (INTERIM).
zephyr_library_sources_ifdef(CONFIG_CAN_ALIF_CAST
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/can/can_cast.c)

zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/storage/sw_fallback.c)
# USB registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/usb_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/usb/zephyr_drv.c)

zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/usb/sw_fallback.c)
# BLE registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/ble_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ble/zephyr_drv.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BLE_CC3501E
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ble/cc3501e.c)

zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/ble/sw_fallback.c)
# Wi-Fi registry sources.  Dispatcher + portable Zephyr backend ship
# unconditionally; sw_fallback gated on the native_sim opt-in.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/wifi_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wifi/zephyr_drv.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_WIFI_CC3501E
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wifi/cc3501e.c)

zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/wifi/sw_fallback.c)
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
# mproc_boot class (alp_mproc_boot_core -- portable peer-core release).
# The dispatcher body + the wildcard NOSUPPORT fallback ride in
# mproc_dispatch.c / mproc/sw_fallback.c above (unconditional, keeps
# the class section populated on every build).  The Alif SE-service
# boot authority gates on its Kconfig (HAS_ALIF_SE_SERVICES).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/mproc/alif_se_boot.c)
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
# SE-CryptoCell hardware-crypto backend (silicon_ref alif:ensemble:e8,
# priority 110 -- outranks the priority-100 "*" PSA zephyr_drv).  Rides the
# hal_alif SE-service MHUv2 -> SE mailbox the read-only se_service client
# already drives live; RNG via the public se_service_get_rnd_num(), the
# AES/SHA/CMAC/CCM/GCM/ChaCha20/Poly1305 wire requests gated behind the
# generic-send seam (CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/security/se_cryptocell.c)
# Update-log surface (<alp/update_log.h>) -- experimental, opt-in.
# Pure engine + software tamper-evident tier + the TF-M secure-service
# client. Gated on CONFIG_ALP_SDK_UPDATE_LOG.  The dispatch owns the
# public surface + the singleton handle; sw_tier registers the universal
# silicon_ref="*" backend; tfm_psa registers only when the build enables
# the TF-M client, and it binds only when a secure owner answers ready().
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_UPDATE_LOG
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log_boot_metadata.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/engine.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/sha256.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/sw_tier.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/tfm_psa.c)
if(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_CLIENT OR CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/update_log/aen_m55_owner.c)
endif()
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_UPDATE_LOG_TFM_PSA_OWNER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/update_log/tfm_psa_secure_owner.c)
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

# No Zephyr-side DRP-AI / DEEPX DX-M1 backends -- deliberately.  Both
# engines are driven from the A55/Linux side only (DRP-AI3 via the
# MERA/TVM runtime, DX-M1 via libdxrt over the A55's PCIe); an M-class
# core cannot reach either, so the former NOT_IMPLEMENTED stubs were
# removed instead of blocking TFLM at priority 100 (issues #58/#59).
# The real bodies live in src/yocto/inference_{drpai,deepx}.cpp.

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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CAMERA_ALIF_ISP
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/alif_isp_pico.c)

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

# E8 LPRTC counter -> calendar shim: backs the alp_rtc_* surface over the
# always-on snps,dw-apb-rtc COUNTER device (lprtc@42000000) -- the E8 has no
# Zephyr-RTC-class peripheral.  Needs the counter subsystem for the device.
if(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8 AND CONFIG_COUNTER)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rtc/lprtc_calendar_shim.c)
endif()

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

# The DSP body + stats surface call CMSIS-DSP kernels (arm_rfft_fast_f32,
# arm_mean/rms/min/max/absmax_f32) when the cmsis-dsp module is linked,
# else the portable-C fallbacks.  The plain-CMake build sets this via
# option(ALP_HAS_CMSIS_DSP); mirror it under Zephyr keyed off
# CONFIG_CMSIS_DSP, otherwise the M55 build silently used portable-C even
# with cmsis-dsp present.  board.yaml `libraries: [cmsis_dsp]` enables the
# full component matrix, so TRANSFORM (arm_rfft_*) + STATISTICS (arm_*_f32)
# link cleanly.  (The macro is only consumed inside the CONFIG_ALP_SDK_DSP
# sources above, so enabling cmsis-dsp without the DSP class is inert.)
zephyr_compile_definitions_ifdef(CONFIG_CMSIS_DSP ALP_HAS_CMSIS_DSP=1)

# Portable Madgwick AHRS (<alp/ahrs.h>).  Pure C (libm); opt-in via
# `libraries: [madgwick_ahrs]` (emits CONFIG_ALP_SDK_AHRS).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_AHRS
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/ahrs.c)

# Portable PID control surface (<alp/pid.h>).  Pure C, no vendor deps;
# opt-in via `libraries: [pid]` (emits CONFIG_ALP_SDK_PID).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PID
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/pid.c)

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
# Operating-point-profile class ("power_profile", handle-less).  The
# dispatcher body + the wildcard NOSUPPORT stub ride in
# power_dispatch.c / power/zephyr_stub.c above (same ALP_SDK_POWER
# gate, so the class section is populated whenever the dispatcher is).
# The Alif SE aiPM backend gates on its Kconfig (HAS_ALIF_SE_SERVICES).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/power/alif_se_profile.c)

# Display class dispatcher + stub backend.  The dispatcher and the
# wildcard stub ship unconditionally so alp_display_open keeps
# linking (and degrading cleanly) on every supported SoC; the real
# Zephyr display driver-class wrapper is gated on its Kconfig
# (default y when CONFIG_DISPLAY provides the display_* symbols).
# Vendor-specific paths (V2N DSI / parallel-RGB, Alif LCD-IF) still
# land per issue #23.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/display_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/display/zephyr_stub.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_DISPLAY_ZEPHYR_DRV
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/display/zephyr_drv.c)

# <alp/gui.h> guard-clause implementation.  OS-agnostic (no Zephyr/
# vendor headers), so compiled unconditionally like cap_helpers.c --
# the real LVGL hand-off is still unimplemented (issue #23), but the
# NULL/NOSUPPORT contract paths must link on every build.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/gui_lvgl_stub.c)

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

# gfx_compat -- maintainer-written in-tree shim (west.yml documents it
# ships at src/lib/gfx_compat/, no west pin).  A native library apps
# call directly (`#include "gfx_compat.h"`), NOT an <alp/gpu2d.h>
# backend -- unrelated to the class dispatcher above.  Compiled
# whenever CONFIG_ALP_GFX_COMPAT_SW is on (see
# src/lib/gfx_compat/Kconfig, default y); the include dir is
# published unconditionally so the header always resolves even if a
# future HW-backend Kconfig replaces the SW default.
add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/src/lib/gfx_compat
    ${CMAKE_CURRENT_BINARY_DIR}/alp_sdk_gfx_compat)

# Runtime hardware-info reader (EEPROM manifest + BOARD_ID ADC).
# v0.3 stub returns NOSUPPORT; v0.3.x fills in the real impl.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_HW_INFO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/hw_info_zephyr.c)

# SoC-identity class ("soc_info" -- the alp_soc_info_read /
# alp_soc_secure_fw_ping half of <alp/hw_info.h>).  Dispatcher +
# wildcard sw_fallback ship unconditionally: the dispatcher stamps the
# build-time soc_ref on every build (native_sim included) and the
# fallback keeps the class section populated.  The Alif SE-service
# identity backend gates on its Kconfig (HAS_ALIF_SE_SERVICES).
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/soc_info_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/soc_info/sw_fallback.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_SOC_INFO_ALIF_SE
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/soc_info/alif_se.c)

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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_PCA9451A
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/pca9451a/pca9451a.c)
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
# `extras-lwrb-nanopb` group is enabled the upstream sources end up at
# modules/lib/lwrb/src/include/ and (once registered as a Zephyr
# module via EXTRA_ZEPHYR_MODULES) win the include search ahead of
# our stub; the ABI is identical so the SDK source is unchanged
# across the swap.  Interim/deferred as of v0.9 -- no committed
# release date for the swap.
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
# alp-sdk-owned dt-bindings headers (e.g. clock/alif-ensemble-clocks-ext.h,
# timer/alif_utimer.h) live under zephyr/include and are #included by C drivers
# (the utimer PWM/counter drivers), not just by DTS -- put the dir on the C path.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/dma)
if(DEFINED ZEPHYR_ALIF_MODULE_DIR)
  zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
endif()
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

# nanopb placeholder framing for <alp/mproc.h>.  Always compiled so
# the failure-path tests link; only invoked at runtime when
# CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y.  Replaced by the
# nanopb-generated codec when the `extras-lwrb-nanopb` west group
# lands -- interim/deferred as of v0.9, no committed release date.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/proto/alp_mproc_frame.c
)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/nanopb/include)

# Header-only 3p libraries vendored in-tree (Task 0 of the
# 3p-library-examples batch) because the west topdir workspace this
# SDK builds against cannot fetch a worktree-only west.yml pin (see
# vendors/{etl,fmt,doctest,jsmn,minimp3}/README.md).  Each profile
# header directory is also added so `#include "etl_profile.h"` /
# `#include "fmt_config.h"` / `#include "doctest_config.h"` resolve
# ahead of any app-local default (metadata/library-profiles/README.md's
# "profile wins over upstream defaults" contract).  jsmn + minimp3
# already carry an `extras-tier1` west pin (unfetched in this
# workspace) -- vendored the same way since both are genuinely
# single-header libraries; no profile header exists for either (their
# `libraries:` Kconfig entry is pure-SW, no tunable defines).
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/etl/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/metadata/library-profiles/etl)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/fmt/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/metadata/library-profiles/fmt)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/doctest/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/metadata/library-profiles/doctest)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/jsmn/include)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/minimp3/include)

# u8g2's minimal core draw-pipeline subset (vendors/u8g2/README.md --
# no zephyr/module.yml upstream, same "genuinely single-header-free but
# small" case as the header-only libs above, except this one needs
# compiled sources). Gated on CONFIG_ALP_SDK_U8G2_VENDORED_CORE, NOT
# CONFIG_ALP_U8G2_SW_BLIT -- the latter `default y`s for EVERY build
# (a fallback-capability marker, true whether or not an app selected
# u8g2), so gating real compiled sources on it broke every other
# example's link (undefined `u8x8_*`/`u8g2_*` symbols never called, but
# worse: it also silently pulled Catch2's C++ TU into C-only apps
# before this was caught -- see the Kconfig symbol's help text). Only
# a slice with `libraries: [u8g2]` sets CONFIG_ALP_SDK_U8G2_VENDORED_CORE
# (scripts/alp_project_emit.py's `_LIBRARY_KCONFIG["u8g2"]`);
# examples/display/u8g2-oled-draw is the first (and so far only)
# consumer.
zephyr_include_directories_ifdef(CONFIG_ALP_SDK_U8G2_VENDORED_CORE
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc
)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_U8G2_VENDORED_CORE
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8x8_setup.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8x8_display.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8x8_8x8.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_setup.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_buffer.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_hvline.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_ll_hvline.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_box.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_font.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_kerning.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/u8g2/csrc/u8g2_intersection.c
)

# Catch2's amalgamated single-TU distribution (vendors/catch2/README.md
# -- no zephyr/module.yml upstream, same case as u8g2 above). Gated on
# CONFIG_ALP_SDK_CATCH2_VENDORED, NOT CONFIG_ALP_CATCH2_SW (see the
# u8g2 comment above -- same `default y`-for-every-build trap, except
# for Catch2 it's worse: a C-only app unconditionally picking up a C++
# TU needing `operator new`/`operator delete` fails EVERY plain-C
# example's link, not just this one). Only a slice with
# `libraries: [catch2]` sets CONFIG_ALP_SDK_CATCH2_VENDORED
# (scripts/alp_project_emit.py's `_LIBRARY_KCONFIG["catch2"]`);
# examples/testing/catch2-selftest is the first (and so far only)
# consumer.
zephyr_include_directories_ifdef(CONFIG_ALP_SDK_CATCH2_VENDORED
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/catch2/include
)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CATCH2_VENDORED
    ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/catch2/src/catch_amalgamated.cpp
)
# Mirrors examples/testing/catch2-selftest/CMakeLists.txt's
# target_compile_definitions(app ...) -- both catch_amalgamated.cpp
# (compiled here, into the alp-sdk module library) and the app's own
# main.cpp (which #includes catch_amalgamated.hpp) must see the SAME
# macro state, or the two TUs parse the shared header differently
# (ODR risk). See that CMakeLists.txt's comment for what each macro
# does.
zephyr_library_compile_definitions_ifdef(CONFIG_ALP_SDK_CATCH2_VENDORED
    CATCH_AMALGAMATED_CUSTOM_MAIN
    CATCH_CONFIG_NO_POSIX_SIGNALS
)

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
# Declare the custom alp_backends_* / alp_backend_classes sections in
# the rodata region.  Required on real boards: Zephyr's ld-script
# targets link with --orphan-handling=warn + --fatal-warnings, so the
# registry sections MUST be placed explicitly (native_sim's host
# linker script auto-places them as orphans, which is why this never
# surfaced in CI).  See the maintenance note in the snippet: one line
# per backend class.
zephyr_linker_sources(ROM_SECTIONS
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/linker/alp-backend-sections.ld)

# I2C class dispatcher + backends.
# The dispatcher and zephyr_drv backend compile unconditionally;
# sw_fallback is gated on CONFIG_ALP_SDK_I2C_SW_FALLBACK.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/i2c_dispatch.c)
zephyr_library_sources_ifdef(CONFIG_I2C
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2c/zephyr_drv.c)
# Register-file I2C target helper (<alp/i2c_regfile.h>).  Pure layer
# over alp_i2c_target_open; OS-agnostic, so compiled unconditionally
# like cap_helpers.c.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/i2c_regfile.c)

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
# CC3501E GPIO proxy: routes proxied IOs through the inter-chip bridge,
# delegates the rest to zephyr_drv.  AEN-only (CONFIG_ALP_SDK_GPIO_CC3501E_PROXY).
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPIO_CC3501E_PROXY
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpio/cc3501e_proxy.c)

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
#
# zephyr_drv is the portable wildcard backend (alp-adcN alias ->
# "alp,adc-input" consumer -> adc_channel_setup_dt / adc_read).  A
# vendor exact-match at the same priority still wins the registry
# tiebreak (see alp_backend_select), so compiling it wherever the
# Zephyr ADC subsystem is on is safe -- it only answers when no
# vendor backend matches the active SoC.
zephyr_library_sources_ifdef(CONFIG_ADC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/zephyr_drv.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/alif_e7.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/alif_e8.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_RENESAS_RZV2N_N44
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/gd32_bridge.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_ADC_SW_FALLBACK
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/sw_fallback.c)

# Alif Ensemble xHCI USB host (UHC) driver skeleton -- BENCH-GATED.
# All host bring-up paths are marked TODO(aen401-bench); the driver compiles
# and links but does not drive hardware.  The xHCI register map (§5.3 cap
# regs) and DWC3 G*-register init are grounded from the Alif DFP soc.h
# (USB_BASE 0x48200000 / USB_IRQ_IRQn 101 / GCTL 0xC110) and the xHCI spec.
# uhc_common.h is a private Zephyr header; expose drivers/usb/uhc so the
# driver can include it (the eth_dwmac_alif + i2s_dw precedents).
# Binding: zephyr/dts/bindings/usb/alif,xhci-uhc.yaml.
# ADR 0017 Tier-1.5 (in-tree clean-room skeleton; no fork copy exists).
zephyr_library_sources_ifdef(CONFIG_UHC_XHCI_ALIF
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/usb/uhc/uhc_xhci_alif.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/usb/uhc/xhci_core.c)
if(CONFIG_UHC_XHCI_ALIF)
    zephyr_include_directories(${ZEPHYR_BASE}/drivers/usb/uhc)
endif()

# <alp/testing/*> virtual-backend test doubles (epic #610).  Hard-gated
# on CONFIG_ZTEST at the Kconfig layer (zephyr/kconfigs/testing.kconfig);
# FATAL_ERROR here too so a workspace that somehow forces
# CONFIG_ALP_SDK_TESTING=y without ZTEST fails the build loudly instead
# of silently shipping a priority-255 wildcard backend that would
# hijack every real board's peripheral selection.
if(CONFIG_ALP_SDK_TESTING)
    if(NOT CONFIG_ZTEST)
        message(FATAL_ERROR
            "CONFIG_ALP_SDK_TESTING requires CONFIG_ZTEST -- the "
            "<alp/testing/*> doubles register priority-255 wildcard "
            "backends that must never ship in a non-test image.")
    endif()

    # include-testing/ is a SEPARATE include root from the public SDK
    # (${ZEPHYR_CURRENT_MODULE_DIR}/include, added unconditionally
    # above) -- only test builds see <alp/testing/*>.
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include-testing)

    zephyr_library_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/testing)

    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/testing/virtual_clock.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/testing/instance_table.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/testing/reset_registry.c)

    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_GPIO
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpio/testing_drv.c)

    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_UART
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/uart/testing_drv.c)

    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_I2C
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/i2c/testing_drv.c)

    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_SPI
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/spi/testing_drv.c)
    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_ADC
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/testing_drv.c)
    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_CAN
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/can/testing_drv.c)
    zephyr_library_sources_ifdef(CONFIG_ALP_SDK_TESTING_STORAGE
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/storage/testing_drv.c)
endif()

# ---------------------------------------------------------------------------
# Alp-maintainer strict-warning profile (issue #634).
#
# Applied PER-SOURCE-FILE, not with a blanket zephyr_library_compile_options()
# -- the alp_sdk zephyr_library above also carries the Alp-authored driver
# glue under zephyr/drivers/** and the zephyr/soc-bridge/** vendor bridge
# copy (both ADR 0017 Tier-1.5/Tier-2, deliberately kept in their upstream
# Zephyr driver style, never this SDK's own house style -- see
# applying-the-alp-sdk-c-house-style).  A target-wide
# zephyr_library_compile_options() would strict-warn (and, with
# CONFIG_ALP_SDK_WERROR, hard-error) that vendor-shaped code too, which
# breaks the "vendored/upstream code never gets -Werror" rule this profile
# exists to uphold.  Restricting COMPILE_OPTIONS to every source under this
# module's own src/ tree keeps the profile first-party-only.  Must run
# after every zephyr_library_sources()/_ifdef() call above so
# target_property(... SOURCES) sees the complete list.
#
# The flag SET is deliberately a reduced "core" subset of the plain-CMake
# alp_sdk_warnings profile (cmake/alp-sdk-warnings.cmake), not the full
# 12-flag list -- confirmed empirically (issue #634) against a spread of
# first-party TUs (src/zephyr/delay_zephyr.c, last_error.c,
# console/alp_console_gpio.c): every one of the flags below is fine, but
# these four are NOT, because they fire on code physically inside
# Zephyr's OWN headers (Zephyr's top-level include dir is a plain -I,
# not -isystem, so GCC does not suppress diagnostics attributed there):
#   -Wpedantic          empty structs / zero-arg variadic macros in
#                        zephyr/include/zephyr/{sys/sflist.h,
#                        sys/__assert.h,device.h}
#   -Wconversion /
#   -Wsign-conversion    narrowing/sign conversions in
#                        zephyr/include/zephyr/sys/{util.h,atomic.h},
#                        arch/common/*.h
#   -Wcast-align         Zephyr's OWN CONTAINER_OF macro
#                        (zephyr/include/zephyr/sys/util.h)
#   -Wundef              CONFIG_* Kconfig symbols Zephyr's own headers
#                        test with plain `#if CONFIG_FOO` (never
#                        `#if defined(...)`) and which autoconf.h simply
#                        omits when unset -- zephyr/include/zephyr/
#                        {rtio/sqe.h,logging/log.h} et al
#   -Wstrict-prototypes  a non-prototype function declaration in
#                        zephyr/include/zephyr/sys/cbprintf.h
# All of the above is upstream Zephyr's own code, never ours to fix
# (ADR 0017), so the required Zephyr-side profile omits them; per the
# issue's own roadmap ("Establish a required core profile first, then
# evaluate higher-noise checks such as conversion and cast alignment
# separately"), plain-CMake -- which never includes Zephyr headers --
# keeps the full 12-flag set unmodified.
if(CONFIG_ALP_SDK_STRICT_WARNINGS)
    set(_alp_sdk_warn_flags "")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        list(APPEND _alp_sdk_warn_flags
            -Wall -Wextra -Wformat=2 -Wshadow
            -Wmissing-prototypes -Wdouble-promotion)
    else()
        message(WARNING
            "alp-sdk: CONFIG_ALP_SDK_STRICT_WARNINGS has no flag mapping for "
            "CMAKE_C_COMPILER_ID='${CMAKE_C_COMPILER_ID}' -- building without "
            "the strict profile.")
    endif()
    if(_alp_sdk_warn_flags AND CONFIG_ALP_SDK_WERROR)
        list(APPEND _alp_sdk_warn_flags -Werror)
    endif()

    if(_alp_sdk_warn_flags)
        get_target_property(_alp_sdk_all_sources alp_sdk SOURCES)
        foreach(_alp_sdk_src IN LISTS _alp_sdk_all_sources)
            string(FIND "${_alp_sdk_src}" "${ZEPHYR_CURRENT_MODULE_DIR}/src/" _alp_sdk_pos)
            if(_alp_sdk_pos EQUAL 0)
                set_source_files_properties(${_alp_sdk_src} PROPERTIES
                    COMPILE_OPTIONS "${_alp_sdk_warn_flags}")
            endif()
        endforeach()
        unset(_alp_sdk_all_sources)
        unset(_alp_sdk_src)
        unset(_alp_sdk_pos)
    endif()
    unset(_alp_sdk_warn_flags)
endif()
