# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the ALP chips library (chips/<part>/...).
#
# Builds libalp_chips.a as a static library that links into
# alp-sdk apps that need on-board chip drivers (LSM6DSO, SSD1306,
# TCAL9538, TAS2563, INA236, CC3501E, etc.).  Each chip driver
# is opt-in via CONFIG_ALP_SDK_CHIP_<NAME>.
#
# The chip drivers are OS-agnostic C; they call back into
# <alp/peripheral.h> which alp-sdk-runtime resolves to Linux V4L2 /
# I2C-dev / spidev / GPIO-cdev backends on the Yocto side.

SUMMARY     = "ALP SDK on-board chip-driver collection"
DESCRIPTION = "Static library of ALP-curated drivers for chips populated \
on the E1M and E1M-X EVKs and modules.  Pairs with libalp_sdk.so."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.1.0+git${SRCPV}"

S = "${WORKDIR}/git"

DEPENDS = "alp-sdk-runtime"

inherit cmake

EXTRA_OECMAKE = " \
    -DALP_OS=yocto \
    -DALP_BUILD_CHIPS_ONLY=ON \
"

# Per-machine chip subset.  V2N / V2N-M1 / N93 carriers all use
# the same E1M EVK, so the chip set is identical -- only the
# peripheral routing underneath differs (handled by
# alp-sdk-runtime).
PACKAGECONFIG ??= " \
    tcal9538 ina236 tas2563 lsm6dso bmi323 bmp581 \
    icm42670 ssd1306 ssd1331 tmp112 rv3028c7 \
    optiga_trust_m eeprom_24c128 ov5640 cam_mux_pi3wvr626 \
"

PACKAGECONFIG[tcal9538]            = "-DALP_SDK_CHIP_TCAL9538=ON,-DALP_SDK_CHIP_TCAL9538=OFF"
PACKAGECONFIG[ina236]              = "-DALP_SDK_CHIP_INA236=ON,-DALP_SDK_CHIP_INA236=OFF"
PACKAGECONFIG[tas2563]             = "-DALP_SDK_CHIP_TAS2563=ON,-DALP_SDK_CHIP_TAS2563=OFF"
PACKAGECONFIG[lsm6dso]             = "-DALP_SDK_CHIP_LSM6DSO=ON,-DALP_SDK_CHIP_LSM6DSO=OFF"
PACKAGECONFIG[bmi323]              = "-DALP_SDK_CHIP_BMI323=ON,-DALP_SDK_CHIP_BMI323=OFF"
PACKAGECONFIG[bmp581]              = "-DALP_SDK_CHIP_BMP581=ON,-DALP_SDK_CHIP_BMP581=OFF"
PACKAGECONFIG[icm42670]            = "-DALP_SDK_CHIP_ICM42670=ON,-DALP_SDK_CHIP_ICM42670=OFF"
PACKAGECONFIG[ssd1306]             = "-DALP_SDK_CHIP_SSD1306=ON,-DALP_SDK_CHIP_SSD1306=OFF"
PACKAGECONFIG[ssd1331]             = "-DALP_SDK_CHIP_SSD1331=ON,-DALP_SDK_CHIP_SSD1331=OFF"
PACKAGECONFIG[tmp112]              = "-DALP_SDK_CHIP_TMP112=ON,-DALP_SDK_CHIP_TMP112=OFF"
PACKAGECONFIG[rv3028c7]            = "-DALP_SDK_CHIP_RV3028C7=ON,-DALP_SDK_CHIP_RV3028C7=OFF"
PACKAGECONFIG[optiga_trust_m]      = "-DALP_SDK_CHIP_OPTIGA_TRUST_M=ON,-DALP_SDK_CHIP_OPTIGA_TRUST_M=OFF"
PACKAGECONFIG[eeprom_24c128]       = "-DALP_SDK_CHIP_EEPROM_24C128=ON,-DALP_SDK_CHIP_EEPROM_24C128=OFF"
PACKAGECONFIG[ov5640]              = "-DALP_SDK_CHIP_OV5640=ON,-DALP_SDK_CHIP_OV5640=OFF"
PACKAGECONFIG[cam_mux_pi3wvr626]   = "-DALP_SDK_CHIP_CAM_MUX_PI3WVR626=ON,-DALP_SDK_CHIP_CAM_MUX_PI3WVR626=OFF"

FILES:${PN}-staticdev = "${libdir}/libalp_chips.a"
FILES:${PN}-dev       = "${includedir}/alp/chips/"
