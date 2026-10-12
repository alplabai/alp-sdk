// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alp Lab AB
//
// Warnings printed by `dxrt-cli -u` / `-w` / `-C` on Alp Lab E1M-V2M modules.
// Nothing is refused or changed: the command then runs exactly as upstream.
// The dx-rt bbappend adds this file to the dx-rt library and inserts one call
// in the constructor of each of those three CLI commands.

#include <iostream>

namespace dxrt {

// kind 0: firmware update / upload, kind 1: firmware configuration
void AlpWarnFwWrite(int kind)
{
    if (kind == 0) {
        std::cerr
            << "WARNING: this module ships with an Alp Lab specific DX-M1 firmware build.\n"
               "Stock DEEPX firmware does not work on this module and leaves the NPU\n"
               "unusable. Recovery is not possible in the field. Continue only with\n"
               "firmware supplied by Alp Lab for this module."
            << std::endl;
    } else {
        std::cerr
            << "WARNING: the DX-M1 firmware configuration (including thermal throttling) is\n"
               "being changed; a wrong configuration can make the NPU unusable."
            << std::endl;
    }
}

}  // namespace dxrt
