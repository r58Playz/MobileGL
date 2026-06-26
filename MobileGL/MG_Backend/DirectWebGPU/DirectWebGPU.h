// MobileGL - MobileGL/MG_Backend/DirectWebGPU/DirectWebGPU.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "MG_State/GLState/TextureState/TextureEnum.h"

namespace MobileGL::MG_State::GLState {
    class ITextureObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend::DirectWebGPU {
    class WebGPURenderer;

    // The active renderer instance (parallel to DirectVulkan's pVulkanRenderer).
    extern UniquePtr<WebGPURenderer> pWebGPURenderer;

    // Free functions wired into the GlobalBackendFunctionsTable. They delegate to
    // the active renderer (and no-op safely before it exists).
    void Clear(GLbitfield mask);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance);
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                               GLsizei instancecount);
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex);
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance);
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                     const void* indices, GLsizei instancecount,
                                                     GLint basevertex, GLuint baseinstance);
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                     const GLvoid* const* indices, GLsizei drawcount,
                                     const GLint* basevertex);
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount);
    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                         GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                    void* pixels);
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                         TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                         GLsizei bufSize, GLvoid* pixels);
    void Finish();
    void Present();
} // namespace MobileGL::MG_Backend::DirectWebGPU
