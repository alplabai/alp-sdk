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
# lockstep).  Limit on dx-rt 3.2.0 even with dxrtd: processes sharing
# one DX-M1 must use the same NPU core set (see <alp/ext/deepx/inference.h>).
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

# DX-M1 firmware on Alp Lab E1M-V2M modules is an Alp-specific no-PMIC build
# (fw 2.4.0), written at the factory. Stock DEEPX firmware leaves the NPUs
# dead (dxrt_polling_ack: timeout) and recovery needs the ROM-UART path, so
# `dxrt-cli -u` (fwupdate), `-w` (fwupload) and `-C` (fwconfig_json) print a
# warning to stderr before running. Nothing is refused and nothing else
# changes: the command proceeds exactly as upstream, and the library entry
# points (Configuration::SetFWConfigWithJson, the Python package) are not
# touched, so profiling works as upstream.
#
# alp_fw_warning.cpp is Alp code, added to the library (lib/ is globbed).
# The constructor of each of the three commands gets one inserted call; the
# edit is a sed on the constructor names, not a patch, so no DEEPX source is
# reproduced here. It fails the build when a constructor is not found, which
# is what happens if a dx-rt pin bump renames them (this "%" append covers
# every PV in the layer; only 3.2.0, SRCREV 6a0052e, was checked).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://alp_fw_warning.cpp"

do_configure:prepend() {
	install -m 0644 ${WORKDIR}/alp_fw_warning.cpp ${S}/lib/alp_fw_warning.cpp
	# do_configure can re-run without a fresh unpack, when ${S}/lib/cli.cpp is
	# already edited; the guard keeps the calls from being inserted twice.
	if [ "$(grep -c "AlpWarnFwWrite(" ${S}/lib/cli.cpp)" != "3" ]; then
		awk '
			/^FWUpdateCommand::FWUpdateCommand\(/ { call = "0" }
			/^FWUploadCommand::FWUploadCommand\(/ { call = "0" }
			/^FWConfigCommandJson::FWConfigCommandJson\(/ { call = "1" }
			{ print }
			call != "" && $0 == "{" { print "    void AlpWarnFwWrite(int); AlpWarnFwWrite(" call ");"; call = "" }
		' ${S}/lib/cli.cpp > ${S}/lib/cli.cpp.alp
		n=$(grep -c "AlpWarnFwWrite(" ${S}/lib/cli.cpp.alp || true)
		if [ "$n" != "3" ]; then
			bbfatal "dx-rt: expected the three CLI command constructors in lib/cli.cpp (inserted $n of 3 calls); update this bbappend for the new dx-rt version"
		fi
		mv ${S}/lib/cli.cpp.alp ${S}/lib/cli.cpp
	fi
}
