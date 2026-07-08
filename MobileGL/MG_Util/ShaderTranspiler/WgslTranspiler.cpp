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
#include "src/tint/lang/wgsl/reserved_words.h"
#include "src/tint/lang/wgsl/writer/writer.h"

namespace MobileGL::MG_Util::ShaderTranspiler {
    namespace {
        void EnsureTintInitialized() {
            static std::once_flag once;
            std::call_once(once, [] { tint::Initialize(); });
        }

        // tint's SPIR-V reader preserves the original GLSL identifier names (via SPIR-V
        // OpName). Some — e.g. `ref` in Iris's SSR code — are WGSL reserved keywords, which
        // this tint version's writer does not rename, so the browser rejects the module.
        // Rename every whole-word identifier that is a WGSL reserved word (append "_rsvd").
        // Reserved words never appear in valid WGSL syntax, so any occurrence is a user
        // identifier; renaming all occurrences the same way preserves semantics. Only the
        // reserved set is touched, so meaningful names (MGL_GLOBAL_UBO, uniform/sampler
        // names the backend matches on) are left intact.
        void SanitizeReservedWords(std::string& wgsl) {
            auto isStart = [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
            };
            auto isPart = [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '_';
            };
            std::string out;
            out.reserve(wgsl.size() + 64);
            for (size_t i = 0, n = wgsl.size(); i < n;) {
                if (isStart(wgsl[i])) {
                    size_t j = i + 1;
                    while (j < n && isPart(wgsl[j])) ++j;
                    std::string_view tok(wgsl.data() + i, j - i);
                    out.append(tok.data(), tok.size());
                    if (tint::wgsl::IsReserved(tok)) {
                        out.append("_rsvd");
                    }
                    i = j;
                } else {
                    out.push_back(wgsl[i]);
                    ++i;
                }
            }
            wgsl.swap(out);
        }
    } // namespace

    WgslResult SpirvToWgsl(const std::vector<uint32_t>& spirv) {
        EnsureTintInitialized();

        WgslResult out;
        if (spirv.empty()) {
            out.error = "empty SPIR-V module";
            return out;
        }

        // Iris shaderpacks do PCF shadow sampling (texture(sampler2DShadow, ...) ->
        // implicit-LOD textureSampleCompare) inside non-uniform control flow (PCF loops,
        // per-pixel branches). WGSL's uniformity analysis rejects derivative/implicit-LOD
        // sampling there, which would fail every terrain/composite program. Allow it: the
        // reader emits `enable chromium_disable_uniformity_analysis;` so the generated WGSL
        // is valid (uniformity becomes a warning, matching desktop GL semantics).
        tint::spirv::reader::Options readerOptions;
        readerOptions.allow_non_uniform_derivatives = true;
        readerOptions.allowed_features = tint::wgsl::AllowedFeatures::Everything();
        tint::Program program = tint::spirv::reader::Read(spirv, readerOptions);
        if (!program.IsValid()) {
            out.error = "tint SPIR-V reader produced an invalid program: " + program.Diagnostics().Str();
            return out;
        }

        auto generated = tint::wgsl::writer::Generate(program, {});
        if (generated != tint::Success) {
            out.error = "tint WGSL writer failed: " + generated.Failure().reason;
            return out;
        }

        out.success = true;
        out.wgsl = std::move(generated.Get().wgsl);
        SanitizeReservedWords(out.wgsl);
        return out;
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
