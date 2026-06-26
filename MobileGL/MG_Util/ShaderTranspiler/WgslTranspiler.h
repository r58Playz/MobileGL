// MobileGL - MobileGL/MG_Util/ShaderTranspiler/WgslTranspiler.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// NOTE: This header is intentionally free of MobileGL's <Includes.h> and of any
// tint headers. The implementation (WgslTranspiler.cpp) is the *only* TU that
// includes tint, and it is compiled with tint's ABI flags (-fno-rtti
// -fno-exceptions -std=c++20). Keeping this interface on plain std types lets the
// rest of MobileGL (C++23, RTTI/exceptions on) call it without ABI concerns.
#include <cstdint>
#include <string>
#include <vector>

namespace MobileGL::MG_Util::ShaderTranspiler {
    struct WgslResult {
        bool success = false;
        std::string wgsl;   // valid when success == true
        std::string error;  // valid when success == false
    };

    // Translate a SPIR-V module (word stream) to WGSL using Dawn's tint
    // (SPIR-V reader -> WGSL writer). Used by the DirectWebGPU backend, since
    // browser WebGPU accepts only WGSL. tint is initialized once on first call.
    WgslResult SpirvToWgsl(const std::vector<uint32_t>& spirv);
} // namespace MobileGL::MG_Util::ShaderTranspiler
