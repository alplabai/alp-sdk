/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/tensor.h.
 * NOT vendor source -- see datatype.h in this directory for why.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dxrt/datatype.h"

namespace dxrt
{

class Tensor
{
  public:
	void *data() const
	{
		return data_;
	}
	std::size_t size_in_bytes() const
	{
		return size_bytes_;
	}
	DataType type() const
	{
		return type_;
	}
	const std::vector<int64_t> &shape() const
	{
		return shape_;
	}

	void                *data_       = nullptr;
	std::size_t          size_bytes_ = 0;
	DataType             type_       = FLOAT;
	std::vector<int64_t> shape_;
};

using Tensors    = std::vector<Tensor>;
using TensorPtrs = std::vector<Tensor *>;

} /* namespace dxrt */
