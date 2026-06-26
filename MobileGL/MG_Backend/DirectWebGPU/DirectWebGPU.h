// MobileGL - MobileGL/MG_Backend/DirectWebGPU/DirectWebGPU.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectWebGPU {
    class WebGPURenderer;

    // The active renderer instance (parallel to DirectVulkan's pVulkanRenderer).
    extern UniquePtr<WebGPURenderer> pWebGPURenderer;

    // Free functions wired into the GlobalBackendFunctionsTable. They delegate to
    // the active renderer (and no-op safely before it exists).
    void Clear(GLbitfield mask);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void Present();
} // namespace MobileGL::MG_Backend::DirectWebGPU
