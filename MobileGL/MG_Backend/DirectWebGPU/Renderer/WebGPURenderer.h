// MobileGL - MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include "../WgpuApi.h"

namespace MobileGL::MG_State::GLState {
    class ProgramObject;
    class VertexArrayObject;
    class BufferObject;
    class ITextureObject;
    class TextureObjectMipmap;
    class SamplerObject;
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
        // Synchronous readback of the default framebuffer (the offscreen color
        // target). Blocks the caller via JSPI until the GPU copy is mapped. Must be
        // reached from a WebAssembly.promising entry (the host swapBuffers path, or
        // the standalone harness' wrapped export).
        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                        void* pixels);
        // Synchronous texture readback (glGetTexImage / glGetTextureImage). Same JSPI
        // path as ReadPixels but reads a texture's own pixels (no framebuffer flip).
        void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels);
        void GetTextureImage(MG_State::GLState::ITextureObject& texture, TextureUploadTarget uploadTarget,
                             GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
        // Blocks (via JSPI) until all submitted GPU work has completed.
        void Finish();

        WGPUDevice GetDevice() const { return m_device; }
        Bool IsInitialized() const { return m_device != nullptr; }

    private:
        // A GLSL combined sampler2D at SPIR-V binding N: tint splits it into a
        // texture at @binding(N) and a sampler at @binding(N+1) (group 0).
        struct SamplerRef {
            Uint32 textureBinding = 0;
            Uint32 samplerBinding = 0;
            String name; // sampler uniform name, e.g. "tex0"
        };

        struct WgpuProgram {
            WGPUShaderModule vertex = nullptr;
            WGPUShaderModule fragment = nullptr;
            // Resource bindings reflected (SPIRV-Reflect) from the SPIR-V, matching
            // the @group(0)/@binding(...) that tint emits.
            Int globalUboBinding = -1; // >=0 if the default-block "MGL_GLOBAL_UBO" exists
            Uint globalUboSize = 0;
            Vector<SamplerRef> samplers;
            Bool HasResources() const { return globalUboBinding >= 0 || !samplers.empty(); }
        };

        struct WgpuPipeline {
            WGPURenderPipeline pipeline = nullptr;
            WGPUBindGroupLayout group0Layout = nullptr; // pipeline auto layout (if resources)
            WGPUBuffer uboBuffer = nullptr;             // global-UBO backing buffer (if any)
        };

        struct WgpuTexture {
            WGPUTexture texture = nullptr;
            WGPUTextureView view = nullptr;
            Uint32 width = 0;
            Uint32 height = 0;
            WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        };

        void BeginFrameIfNeeded();
        Bool AcquireSurfaceView();
        void EndFrame();
        // (Re)creates the offscreen color target at the current canvas size. All
        // clears/draws render here; Present copies it to the swapchain texture. This
        // keeps a stable, copyable image for ReadPixels (canvas textures aren't
        // readable after present).
        void EnsureOffscreenTarget();
        // Finishes + submits the in-progress command encoder and opens a fresh one,
        // so already-recorded draws land in the offscreen target before a readback.
        void FlushFrame();
        // Shared readback core: copies an RGBA8-family region of `tex` into `out`
        // (4 bytes/texel) via copyTextureToBuffer + an async map blocked on JSPI.
        // `flipY` reverses rows (framebuffer origin); `srcIsBgra` vs `dstFormat`
        // (GL_RGBA/GL_BGRA) decides an R/B swap. Returns false on failure.
        Bool ReadTextureToCPU(WGPUTexture tex, Uint32 srcX, Uint32 srcY, Uint32 w, Uint32 h,
                              Bool srcIsBgra, GLenum dstFormat, Bool flipY, void* out);

        const WgpuProgram* GetOrCreateProgram(MG_State::GLState::ProgramObject& program);
        const WgpuPipeline* GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                                const MG_State::GLState::VertexArrayObject& vao,
                                                GLenum mode);
        WGPUBuffer GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer);
        WGPUBuffer GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer);
        const WgpuTexture* GetOrCreateTexture(MG_State::GLState::ITextureObject& texture);
        // Uploads mip level 0 (RGBA8) of `mip` into `tex` via the queue. Returns false
        // if the source pixels aren't available.
        Bool UploadTextureLevel0(WGPUTexture tex, MG_State::GLState::TextureObjectMipmap& mip,
                                 Uint32 w, Uint32 h);
        // Builds (or reuses) a WGPUSampler matching the GL sampler params
        // (glTexParameter / glBindSampler). Cached by the resolved param values.
        WGPUSampler GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler);
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
        UnorderedMap<const MG_State::GLState::ITextureObject*, WgpuTexture> m_textureCache;
        // WGPUSamplers keyed by a hash of their resolved descriptor (GL sampler params).
        UnorderedMap<Uint64, WGPUSampler> m_samplerCache;
        // Bind groups reference runtime resources (textures/UBO contents), so they are
        // rebuilt each draw and released at frame end.
        Vector<WGPUBindGroup> m_frameBindGroups;

        // Persistent offscreen color target: all rendering goes here, then Present
        // copies it to the acquired swapchain texture. ReadPixels copies from it.
        WGPUTexture m_offscreenTexture = nullptr;
        WGPUTextureView m_offscreenView = nullptr;
        Uint32 m_offscreenWidth = 0;
        Uint32 m_offscreenHeight = 0;
        // Guards against reentrant readback while a JSPI suspension is in flight.
        Bool m_readbackInFlight = false;

        // Per-frame transient state (valid only between BeginFrameIfNeeded and Present)
        WGPUCommandEncoder m_encoder = nullptr;
        WGPUTexture m_frameTexture = nullptr;
        WGPUTextureView m_frameView = nullptr;
        Bool m_frameActive = false;
    };
} // namespace MobileGL::MG_Backend::DirectWebGPU
