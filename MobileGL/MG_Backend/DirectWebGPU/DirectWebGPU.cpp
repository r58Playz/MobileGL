// MobileGL - MobileGL/MG_Backend/DirectWebGPU/DirectWebGPU.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectWebGPU.h"
#include "Renderer/WebGPURenderer.h"

namespace MobileGL::MG_Backend::DirectWebGPU {
    UniquePtr<WebGPURenderer> pWebGPURenderer;

    void Clear(GLbitfield mask) {
        if (pWebGPURenderer) {
            pWebGPURenderer->Clear(mask);
        }
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawArrays(mode, first, count);
        }
    }

    void Present() {
        if (pWebGPURenderer) {
            pWebGPURenderer->Present();
        }
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
