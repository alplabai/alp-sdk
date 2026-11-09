# SPDX-License-Identifier: Apache-2.0
#
# LGPL-only ffmpeg: no GPL code and no x264. A bbappend applies to every ffmpeg
# built with this layer (any opencv/libav consumer too), not just
# packagegroup-alp-multimedia.
# libpostproc is GPL-2.0-or-later (ffmpeg.bb sets LICENSE:libpostproc), so it
# goes too. ffmpeg stays LICENSE_FLAGS = "commercial" (patent-encumbered
# codecs): the integrator accepts commercial_ffmpeg, see docs/v2n-multimedia.md.
#
# Copyright (C) 2026 Alp Lab AB

PACKAGECONFIG:remove = "gpl x264 postproc"
# poky sets LICENSE:${PN} = GPL-2.0-or-later unconditionally; without gpl the CLI is LGPL.
LICENSE:${PN} = "LGPL-2.1-or-later"
