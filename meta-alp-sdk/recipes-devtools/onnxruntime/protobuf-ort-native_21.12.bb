# SPDX-License-Identifier: Apache-2.0
#
# Host protoc 21.12 for onnxruntime_1.28.0.bb -- and ONLY for it.
#
# ORT 1.28.0's cmake/deps.txt pins protobuf v21.12, which predates
# protobuf's abseil dependency, so ORT builds a self-contained static
# libprotobuf-lite from that source (see the ABI note in the onnxruntime
# recipe).  protobuf's generated *.pb.h/.pb.cc embed a version check
# against the runtime headers, so the protoc that generates ORT's and
# onnx's sources must be 21.12 too; the distro's protobuf-native (4.25.8)
# emits code the 21.12 headers reject.
#
# Installed under ${libdir}/protobuf-ort so it never shadows the distro
# protoc that other recipes resolve from ${bindir}.

SUMMARY = "protoc 21.12 host compiler pinned to ONNX Runtime's bundled protobuf"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=37b5762e07f0af8c74ce80a8bda4266b"

SRC_URI = "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.zip;downloadfilename=protobuf-21.12.zip;sha1sum=7cf2733949036c7d52fda017badcab093fe73bfa"
SRC_URI[sha256sum] = "6a31b662deaeb0ac35e6287bda2f3369b19836e6c9f8828d4da444346f420298"

S = "${WORKDIR}/protobuf-21.12"
OECMAKE_SOURCEPATH = "${S}/cmake"

inherit native cmake

EXTRA_OECMAKE += " \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_WITH_ZLIB=OFF \
"

do_install() {
    install -d ${D}${libdir}/protobuf-ort
    install -m 0755 ${B}/protoc ${D}${libdir}/protobuf-ort/protoc
}
