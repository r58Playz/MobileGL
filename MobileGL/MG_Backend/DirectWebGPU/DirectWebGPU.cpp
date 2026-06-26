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

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawElements(mode, count, type, indices);
        }
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                    void* pixels) {
        if (pWebGPURenderer) {
            pWebGPURenderer->ReadPixels(x, y, width, height, format, type, pixels);
        }
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        if (pWebGPURenderer) {
            pWebGPURenderer->GetTexImage(target, level, format, type, pixels);
        }
    }

    void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                         TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                         GLsizei bufSize, GLvoid* pixels) {
        if (pWebGPURenderer && texture) {
            pWebGPURenderer->GetTextureImage(*texture, uploadTarget, level, format, type, bufSize, pixels);
        }
    }

    void Finish() {
        if (pWebGPURenderer) {
            pWebGPURenderer->Finish();
        }
    }

    void Present() {
        if (pWebGPURenderer) {
            pWebGPURenderer->Present();
        }
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
