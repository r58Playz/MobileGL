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

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawArraysInstanced(mode, first, count, instancecount, 0);
        }
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawArraysInstanced(mode, first, count, instancecount, baseinstance);
        }
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                               GLsizei instancecount) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawElementsInstanced(mode, count, type, indices, instancecount, 0, 0);
        }
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawElementsInstanced(mode, count, type, indices, instancecount, basevertex, 0);
        }
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawElementsInstanced(mode, count, type, indices, instancecount, 0, baseinstance);
        }
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                     const void* indices, GLsizei instancecount,
                                                     GLint basevertex, GLuint baseinstance) {
        if (pWebGPURenderer) {
            pWebGPURenderer->DrawElementsInstanced(mode, count, type, indices, instancecount, basevertex,
                                                   baseinstance);
        }
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                     const GLvoid* const* indices, GLsizei drawcount,
                                     const GLint* basevertex) {
        if (pWebGPURenderer) {
            pWebGPURenderer->MultiDrawElementsBaseVertex(mode, count, type, indices, drawcount, basevertex);
        }
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {
        if (pWebGPURenderer) {
            pWebGPURenderer->MultiDrawElementsBaseVertex(mode, count, type, indices, drawcount, nullptr);
        }
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                         GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        if (pWebGPURenderer) {
            pWebGPURenderer->BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                             filter);
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
