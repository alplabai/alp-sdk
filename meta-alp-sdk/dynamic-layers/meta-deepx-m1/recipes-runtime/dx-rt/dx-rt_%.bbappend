# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# meta-deepx-m1's dx-rt recipes declare `PACKAGES:append = " ${PN}-cli
# ${PN}-examples"`, which puts both sub-packages AFTER ${PN}.  Packaging
# hands each file to the FIRST package whose FILES matches, and ${PN}'s
# default FILES already covers ${bindir}/*, so dxrt-cli / dxrtd /
# run_model / dxtop / dxbenchmark and bin/examples/* all land in dx-rt and
# dx-rt-cli is never written (empty, not ALLOW_EMPTY).  An image that
# installs dx-rt-cli (e1m-v2m-deepx.inc) then fails do_rootfs with
#     Error: Unable to find a match: dx-rt-cli
# Drop the ${bindir}/* glob from ${PN} so those files fall through to the
# sub-packages' own FILES.  Every ${bindir} file dx-rt 3.2.0 installs is
# named by FILES:${PN}-cli or matched by FILES:${PN}-examples, so nothing
# is left unshipped.  (Reordering PACKAGES instead does not work from a
# bbappend: :remove would also strip the recipe's own :append entries,
# and listing a package twice is a packaging error.)
FILES:${PN}:remove = "${bindir}/*"

# #2398: run DX-M1 clients through the dxrtd service.  dx-rt_3.2.0.bb
# builds -DUSE_SERVICE=OFF, so two processes using the DX-M1 at once
# hang (no daemon arbitrates the device) and a client killed mid-request
# wedges the NPU until reboot.  Build the client library in service mode
# and start dxrtd at boot.  Upstream's dx-rt_3.2.0-1.bb does the same
# but ships only a SysV init script (its dxrt.service is commented out);
# this image runs systemd, so install upstream's unit instead.  The
# PREFERRED_VERSION pin in e1m-v2m-deepx.inc stays on 3.2.0 (firmware
# lockstep).  Limit, even with dxrtd: at most 3 DISTINCT NPU core sets
# live on one DX-M1 at a time (kernel driver DX_NORMAL_QUEUE_MAX = 3; a 4th
# makes dx-rt abort / kill dxrtd).  See <alp/ext/deepx/inference.h>.
EXTRA_OECMAKE:remove = "-DUSE_SERVICE=OFF"
EXTRA_OECMAKE += "-DUSE_SERVICE=ON"

inherit systemd

SRC_URI += "file://dxrt.service"

SYSTEMD_PACKAGES = "${PN}-cli"
SYSTEMD_SERVICE:${PN}-cli = "dxrt.service"
SYSTEMD_AUTO_ENABLE:${PN}-cli = "enable"

do_install:append() {
	install -Dm 0644 ${WORKDIR}/dxrt.service ${D}${systemd_system_unitdir}/dxrt.service
}

FILES:${PN}-cli += "${systemd_system_unitdir}/dxrt.service"
