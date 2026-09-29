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
