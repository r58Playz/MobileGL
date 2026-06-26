// MobileGL - MobileGL/MG_Backend/DirectWebGPU/WgpuApi.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// Thin shim over the WebGPU C API. This is the single place where the
// difference between binding versions (e.g. legacy -sUSE_WEBGPU vs the new
// standardized emdawnwebgpu webgpu.h) is meant to be absorbed, so the rest of the
// DirectWebGPU backend stays binding-version-agnostic. Today it targets the
// vendored emdawnwebgpu headers (included via <Includes.h> under __EMSCRIPTEN__).
// NOTE: do NOT include <emscripten/html5_webgpu.h> — that is Emscripten's legacy
// (-sUSE_WEBGPU) header and is incompatible with emdawnwebgpu's webgpu.h (it
// references the removed WGPUSwapChain). emdawnwebgpu's webgpu.h itself declares
// emscripten_webgpu_get_device().
#include <webgpu/webgpu.h>

#include <cstring>

namespace MobileGL::MG_Backend::DirectWebGPU::Wgpu {
    // Build a WGPUStringView from a NUL-terminated C string (emdawnwebgpu API).
    inline WGPUStringView View(const char* s) {
        WGPUStringView v;
        v.data = s;
        v.length = s ? std::strlen(s) : 0;
        return v;
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU::Wgpu
