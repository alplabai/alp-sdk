# SPDX-License-Identifier: Apache-2.0
#
# systemd watchdog policy for ALP images: PID 1 arms the SoC hardware
# watchdog at early boot and pets it for the life of the system
# (RuntimeWatchdogSec), and re-arms it explicitly for the reboot window
# (RebootWatchdogSec) instead of relying on systemd-shutdown's
# unconfigured 1-minute default.
#
# Pairs with the DT side (e1m-v2n-som.dtsi enables &wdt1 with
# timeout-sec); the kernel driver is the BSP's rzv2h_wdt.  If the BSP
# defconfig builds it =m, PID 1 arms the dog once udev coldplug loads
# the module -- still minutes earlier than the previous
# shutdown-only arming.
#
# VALIDATION: pending bench hang-injection on a built image (arm,
# `echo c > /proc/sysrq-trigger`, expect reset within RuntimeWatchdogSec).

SUMMARY = "systemd hardware-watchdog supervision policy for ALP images"
DESCRIPTION = "Installs a systemd system.conf drop-in that sets \
               RuntimeWatchdogSec/RebootWatchdogSec so the SoC \
               watchdog supervises runtime and reboot, not just the \
               final shutdown window."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://10-alp-watchdog.conf \
"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d ${D}${sysconfdir}/systemd/system.conf.d
    install -m 0644 ${WORKDIR}/10-alp-watchdog.conf \
        ${D}${sysconfdir}/systemd/system.conf.d/10-alp-watchdog.conf
}

FILES:${PN} += " \
    ${sysconfdir}/systemd/system.conf.d/10-alp-watchdog.conf \
"
