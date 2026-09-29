# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# meta-rz-distro's v4l2-init installs /root/v4l2-init.sh plus a unit that
# runs it at boot. The script configures the CRU media pipeline for the
# Renesas EVK's OV5645 camera and exits 1 ("No CRU video device found")
# when that camera is absent, which is every E1M-X EVK today (the camera
# path is still open, #1149), so the unit showed as failed on every boot.
# Keep the script and unit installed for anyone who fits that camera, but
# do not start it automatically (`systemctl enable --now v4l2-init`).
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
