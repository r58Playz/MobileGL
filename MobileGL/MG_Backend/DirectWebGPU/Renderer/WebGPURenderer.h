// MobileGL - MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../WgpuApi.h"

namespace MobileGL::MG_State::GLState {
    class ProgramObject;
    class VertexArrayObject;
    class BufferObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend::DirectWebGPU {
    // Minimal WebGPU renderer: owns the device/queue/canvas surface and implements
    // clear + present (M2a) and a minimal DrawArrays path (M2b: position-style
    // vertex attributes, no uniforms/textures/indices yet). WebGPU has no manual
    // memory/fences/render-pass objects, so this is far smaller than the Vulkan
    // equivalent. The device is owned and used only on this (render) thread.
    class WebGPURenderer {
    public:
        WebGPURenderer() = default;
        ~WebGPURenderer();
        WebGPURenderer(const WebGPURenderer&) = delete;
        WebGPURenderer& operator=(const WebGPURenderer&) = delete;

        Bool Initialize(const String& canvasSelector);
        void Shutdown();

        void Clear(GLbitfield mask);
        void DrawArrays(GLenum mode, GLint first, GLsizei count);
        void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
        void Present();

        WGPUDevice GetDevice() const { return m_device; }
        Bool IsInitialized() const { return m_device != nullptr; }

    private:
        struct WgpuProgram {
            WGPUShaderModule vertex = nullptr;
            WGPUShaderModule fragment = nullptr;
            // Default-block uniforms are packed by glslang into the "MGL_GLOBAL_UBO".
            // For uniform-only shaders glslang auto-maps it to @group(0) @binding(0)
            // (which tint preserves). >=0 means the program has a global UBO.
            // TODO: reflect the binding (SpvcSession) once textures/explicit UBOs land.
            Int globalUboBinding = -1;
            Uint globalUboSize = 0;
        };

        struct WgpuPipeline {
            WGPURenderPipeline pipeline = nullptr;
            // Global-UBO bind group (group 0), created against the pipeline's auto
            // layout; null when the program has no uniforms.
            WGPUBuffer uboBuffer = nullptr;
            WGPUBindGroup bindGroup = nullptr;
            Uint uboSize = 0;
        };

        void BeginFrameIfNeeded();
        Bool AcquireSurfaceView();
        void EndFrame();

        const WgpuProgram* GetOrCreateProgram(MG_State::GLState::ProgramObject& program);
        const WgpuPipeline* GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                                const MG_State::GLState::VertexArrayObject& vao,
                                                GLenum mode);
        WGPUBuffer GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer);
        WGPUBuffer GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer);
        WGPUShaderModule MakeShaderModule(const char* wgsl);
        // Begins a Load render pass, binds the pipeline + vertex buffers for the
        // current program/VAO. Returns the pass (caller draws + ends) or nullptr.
        WGPURenderPassEncoder BeginDrawPass(MG_State::GLState::ProgramObject& program,
                                            const MG_State::GLState::VertexArrayObject& vao, GLenum mode);

        WGPUInstance m_instance = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUQueue m_queue = nullptr;
        WGPUSurface m_surface = nullptr;
        WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
        Uint32 m_width = 0;
        Uint32 m_height = 0;
        String m_canvasSelector;

        // Caches (keyed by the MG_State object identity; minimal invalidation for now).
        UnorderedMap<const MG_State::GLState::ProgramObject*, WgpuProgram> m_programCache;
        UnorderedMap<const MG_State::GLState::ProgramObject*, WgpuPipeline> m_pipelineCache;
        UnorderedMap<const MG_State::GLState::BufferObject*, WGPUBuffer> m_vertexBufferCache;
        UnorderedMap<const MG_State::GLState::BufferObject*, WGPUBuffer> m_indexBufferCache;

        // Per-frame transient state (valid only between BeginFrameIfNeeded and Present)
        WGPUCommandEncoder m_encoder = nullptr;
        WGPUTexture m_frameTexture = nullptr;
        WGPUTextureView m_frameView = nullptr;
        Bool m_frameActive = false;
    };
} // namespace MobileGL::MG_Backend::DirectWebGPU
