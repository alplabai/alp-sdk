# SPDX-License-Identifier: Apache-2.0
#
# LGPL-only ffmpeg for packagegroup-alp-multimedia: no GPL code and no x264.
# libpostproc is GPL-2.0-or-later (ffmpeg.bb sets LICENSE:libpostproc), so it
# goes too. ffmpeg stays LICENSE_FLAGS = "commercial" (patent-encumbered
# codecs): the integrator accepts commercial_ffmpeg, see docs/v2n-multimedia.md.
#
# Copyright (C) 2026 Alp Lab AB

PACKAGECONFIG:remove = "gpl x264 postproc"
