/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/datatype.h.
 *
 * NOT vendor source.  dx_rt is proprietary (DEEPX EULA, customer-only --
 * see src/yocto/inference_deepx.cpp's "Vendor-artifact handling" note) and
 * its real headers cannot be vendored into this public repo or into CI
 * (issue #1747: no PR gate builds this backend for the same reason).  This
 * directory declares only the shape src/yocto/inference_deepx.cpp
 * documents itself as using (its own "Real vendor API" header comment),
 * reconstructed from that documentation alone -- clean-room, same
 * treatment the file already gives the real thing -- so the tests in
 * tests/yocto/inference_deepx_regression.cpp can drive that file's OWN
 * C++ logic deterministically without any real dx_rt install.
 */
#pragma once

namespace dxrt
{

enum DataType {
	NONE = 0,
	FLOAT,
	UINT8,
	INT8,
	UINT16,
	INT16,
	INT32,
	UINT32,
	INT64,
	UINT64,
	BBOX,
	FACE,
	POSE,
};

} /* namespace dxrt */
