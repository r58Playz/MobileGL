// MobileGL - MobileGL/MG_Util/ShaderTranspiler/WgslTranspiler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// This TU is the sole bridge into Dawn's tint. It is compiled with tint's ABI
// flags (-fno-rtti -fno-exceptions -std=c++20) and the TINT_BUILD_* defines, set
// per-file in CMakeLists.txt. It deliberately avoids MobileGL's <Includes.h>.
#include "WgslTranspiler.h"

#include <mutex>

#include "src/tint/api/tint.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/program/program.h"
#include "src/tint/lang/wgsl/writer/writer.h"

namespace MobileGL::MG_Util::ShaderTranspiler {
    namespace {
        void EnsureTintInitialized() {
            static std::once_flag once;
            std::call_once(once, [] { tint::Initialize(); });
        }
    } // namespace

    WgslResult SpirvToWgsl(const std::vector<uint32_t>& spirv) {
        EnsureTintInitialized();

        WgslResult out;
        if (spirv.empty()) {
            out.error = "empty SPIR-V module";
            return out;
        }

        tint::Program program = tint::spirv::reader::Read(spirv, {});
        if (!program.IsValid()) {
            out.error = "tint SPIR-V reader produced an invalid program";
            return out;
        }

        auto generated = tint::wgsl::writer::Generate(program, {});
        if (generated != tint::Success) {
            out.error = "tint WGSL writer failed: " + generated.Failure().reason;
            return out;
        }

        out.success = true;
        out.wgsl = std::move(generated.Get().wgsl);
        return out;
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
