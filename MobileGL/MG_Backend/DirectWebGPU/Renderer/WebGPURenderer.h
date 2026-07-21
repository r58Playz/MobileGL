// MobileGL - MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderObject.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include "../WgpuApi.h"
#include "WebGPUPlatform.h"

namespace MobileGL {
    enum class FramebufferTarget;
} // namespace MobileGL

namespace MobileGL::MG_State::GLState {
    class ProgramObject;
    class VertexArrayObject;
    struct VertexAttribute;
    class BufferObject;
    class ITextureObject;
    class TextureObjectMipmap;
    class SamplerObject;
    class FramebufferObject;
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

        Bool Initialize(const Platform::InitInfo& info);
        void Resize(Uint32 width, Uint32 height);
        void Shutdown();

        void Clear(GLbitfield mask);
        void DrawArrays(GLenum mode, GLint first, GLsizei count);
        void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
        // Instanced draws (all glDraw*Instanced* variants route here). baseVertex is
        // applied via drawIndexed. WebGPU direct draws support a non-zero first instance;
        // only indirect draws require the indirect-first-instance feature.
        void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount,
                                 GLuint baseInstance);
        void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                   GLsizei instanceCount, GLint baseVertex, GLuint baseInstance);
        // glMultiDrawElementsBaseVertex: WebGPU has no multi-draw, so this is a CPU loop
        // of drawIndexed (one per sub-draw) sharing a single render pass.
        void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                         const void* const* indices, GLsizei drawcount,
                                         const GLint* basevertex);
        void Present();
        // glFlush: submit pending GPU commands without blocking, so a long stream of
        // draws doesn't accumulate unbounded in one command buffer.
        void Flush() { FlushFrame(); }
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
        // glBlitFramebuffer: direct copies for identical textures/formats, with a
        // sampled render path for scaling and RGBA/BGRA format conversion.
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                             GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
        // glGenerateMipmap / glGenerateTextureMipmap: fill mip levels 1+ of the bound
        // 2D texture by rendering each level from the previous (linear 2x2 downsample).
        void GenerateMipmaps(GLenum target);
        // Blocks (via JSPI) until all submitted GPU work has completed.
        void Finish();

        WGPUDevice GetDevice() const { return m_device; }
        Bool IsInitialized() const { return m_device != nullptr; }

    private:
        // One texture or sampler binding, parsed from tint's WGSL output (the exact
        // module interface). A GLSL combined sampler2D becomes a texture var and -
        // only if the shader actually samples it (vs texelFetch-only) - a separate
        // sampler var; tint drops the sampler for texelFetch-only textures, so we
        // must read what it really emitted rather than assume N/N+1. Bindings are
        // already stage-offset (see StageBindingOffset). glName is the original GL
        // uniform name (tint's "<name>_image"/"<name>_sampler" with the suffix
        // stripped), used to resolve the bound texture unit.
        struct ResourceRef {
            Uint32 binding = 0;
            enum class Kind { Texture, Sampler } kind = Kind::Texture;
            String glName;
            // Parsed from tint's WGSL type: a Texture may be `texture_depth_2d` (from a
            // GLSL sampler2DShadow) and a Sampler may be `sampler_comparison`. These pick
            // the WebGPU bind-group entry type (depth sample type / comparison sampler).
            Bool wgslDepth = false;
            Bool wgslComparison = false;
            // Parsed as `texture_cube<...>` (from a GLSL samplerCube): resolve the bound
            // texture from the unit's CUBE_MAP slot, bind it through a cube view, and (in an
            // explicit layout) declare the entry's viewDimension as Cube.
            Bool wgslCube = false;
        };

        // A vertex shader input (@location(N) ... : TYPE), parsed from tint's WGSL.
        // The pipeline must declare every one of these, even when the GL VAO has the
        // attribute disabled / in an unsupported format (else CreateRenderPipeline fails
        // and the invalid pipeline poisons the whole command buffer).
        struct VertexInput {
            Uint32 location = 0;
            Bool isInt = false;      // shader reads it as an integer (u32/i32, vs f32)
            Bool isUnsigned = false; // when isInt: u32 (true) vs i32 (false)
        };

        struct WgpuProgram {
            struct StageUboBinding {
                Uint32 binding = 0;
                ShaderStage stage = ShaderStage::Unknown;
                // -1 is the synthetic default block populated by glUniform*. Other
                // values index ProgramObject's named uniform blocks.
                Int blockIndex = -1;
            };

            WGPUShaderModule vertex = nullptr;
            WGPUShaderModule fragment = nullptr;
            // Bindings parsed from tint's WGSL. WebGPU merges all stages into one
            // @group(0), but glslang numbers each stage's resources from 0, so we
            // offset each stage into a disjoint binding range (StageBindingOffset).
            // The default-block UBO appears once per stage that uses it. Each binding
            // must receive that stage's independently reflected layout/payload.
            Vector<StageUboBinding> uboBindings;
            Vector<ResourceRef> resources;
            Vector<VertexInput> vertexLocations;
            // Fragment-stage output @location(N) values parsed from tint's WGSL. Drives
            // the pipeline's color-target array and the render pass' color attachments:
            // WebGPU requires a target for exactly the locations the shader writes.
            Vector<Uint32> fragmentOutputs;
            Bool HasResources() const { return !uboBindings.empty() || !resources.empty(); }
        };

        struct WgpuPipeline {
            WGPURenderPipeline pipeline = nullptr;
            WGPUBindGroupLayout group0Layout = nullptr; // auto layout, or the explicit one below
            // Non-null when this pipeline uses an explicit (hand-built) bind-group layout
            // instead of tint's auto layout — required when a plain sampler2D binds a
            // depth-format texture (WebGPU won't bind depth to a "float" sample type). The
            // explicit bind-group layout itself is held in group0Layout above.
            WGPUPipelineLayout pipelineLayout = nullptr;
            Uint dummyVertexSlots = 0; // # of constant zero vertex buffers bound after the real ones
        };

        struct WgpuTexture {
            WGPUTexture texture = nullptr;
            WGPUTextureView view = nullptr; // sampled view (respects texture level range)
            WGPUTextureView attachmentView = nullptr; // linear render-target view
            WGPUTextureView attachmentSrgbView = nullptr; // sRGB render-target view when enabled
            Uint32 width = 0;
            Uint32 height = 0;
            Uint32 mipLevelCount = 1;
            Uint32 viewBaseMipLevel = 0;
            Uint32 viewMipLevelCount = 1;
            Uint16 textureParamsVersion = 0;
            Uint32 swizzleKey = 0xFFFF; // baked GL swizzle (PackSwizzle); 0xFFFF => none. A
                                        // change forces a re-materialize (full recreate).
            WGPUTextureFormat format = WGPUTextureFormat_Undefined;
            Bool isDepth = false; // depth-format texture (sampled via depth/unfilterable-float)
            // See WgpuBuffer::lastUseSerial — same submit-ordering hazard for
            // queueWriteTexture re-uploads of a texture recorded draws sample or
            // render into.
            Uint64 lastUseSerial = 0;
        };

        // GPU copy of a GL buffer; re-uploaded when the buffer's change serial advances
        // (glBufferData/glBufferSubData/map-flush all bump it), recreated if it grows.
        // lastUseSerial: the m_flushSerial value when this buffer was last handed to a
        // draw. queueWrite* executes at submit time (before the whole pending command
        // buffer), so re-uploading a buffer that already-recorded draws reference must
        // submit those draws first — else they would read the NEW contents (e.g.
        // Minecraft re-fills one dynamic VB between GUI draws every frame).
        struct WgpuBuffer {
            WGPUBuffer buffer = nullptr;
            Uint64 serial = ~Uint64(0);
            Uint64 size = 0;
            Uint64 lastUseSerial = 0;
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
        // Resolves the bound draw framebuffer into the current draw target (m_cur*).
        // Default framebuffer -> the offscreen color+depth; a user FBO -> its color
        // attachment texture (+ a transient depth texture if it has a depth attachment).
        // Returns false if the target can't be resolved (e.g. incomplete FBO).
        Bool ResolveDrawTarget();
        // Resolves a framebuffer's color attachment 0 to a WGPU texture (+ size/format).
        // Default FB -> the offscreen color target; user FBO -> its Color0 texture.
        Bool ResolveColorTexture(FramebufferTarget target, WGPUTexture& outTex, Uint32& outW, Uint32& outH,
                                 WGPUTextureFormat& outFmt);
        // Transient depth buffer for a user FBO (cached, recreated on size change).
        WGPUTextureView GetOrCreateFboDepth(const MG_State::GLState::FramebufferObject& fbo, Uint32 w,
                                            Uint32 h);
        // Shared readback core: copies an RGBA8-family region of `tex` into `out`
        // (4 bytes/texel) via copyTextureToBuffer + an async map blocked on JSPI.
        // `flipY` reverses rows (framebuffer origin); `srcIsBgra` vs `dstFormat`
        // (GL_RGBA/GL_BGRA) decides an R/B swap. Returns false on failure.
        // Reads `tex` into `out` as RGBA8. 8-bit source formats copy directly (with R/B
        // swap per dstFormat); float sources (RGBA16F/RGBA32F/RG11B10F) are decoded and
        // clamped to [0,1]*255 for display/diagnostics.
        Bool ReadTextureToCPU(WGPUTexture tex, Uint32 srcX, Uint32 srcY, Uint32 w, Uint32 h,
                              WGPUTextureFormat srcFmt, GLenum dstFormat, Bool flipY, void* out);

        const WgpuProgram* GetOrCreateProgram(MG_State::GLState::ProgramObject& program);
        const WgpuPipeline* GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                                const MG_State::GLState::VertexArrayObject& vao,
                                                GLenum mode, WGPUIndexFormat stripIndexFormat);
        // Hash of everything WebGPU bakes into a render pipeline for the current GL
        // state, so distinct states map to distinct cached pipelines.
        Uint64 ComputePipelineKey(const MG_State::GLState::ProgramObject& program,
                                  const MG_State::GLState::VertexArrayObject& vao, GLenum mode,
                                  WGPUIndexFormat stripIndexFormat, const WgpuProgram* prog) const;
        // Depth texture creation (sampleable depth-format textures, for depthtex/shadowtex).
        const WgpuTexture* GetOrCreateDepthTexture(MG_State::GLState::ITextureObject& texture,
                                                   WGPUTextureFormat depthFmt);
        WGPUBuffer GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer);
        // Deinterleaved copy of ONE vertex attribute into its own tightly-packed buffer
        // (bindable at offset 0, attribute offset 0, 4-aligned arrayStride). Used when the
        // GL attribute's byte offset or stride isn't 4-aligned — WebGPU requires the
        // SetVertexBuffer offset, attribute offset, and arrayStride all be multiples of 4,
        // which an interleaved GL buffer (e.g. a 2-byte attribute at byte 42) can't satisfy.
        // Cached per (buffer, offset, stride, attrBytes).
        WGPUBuffer GetOrCreateDeinterleavedBuffer(MG_State::GLState::BufferObject& buffer, Uint32 offsetBytes,
                                                  Uint32 strideBytes, Uint32 attrBytes, Uint32 alignedStride);
        // Deinterleave AND widen an integer vertex attribute to float32 (GL converts a
        // non-normalized int attribute read as float; WebGPU has no such vertex format).
        // componentBytes is 1/2/4; isSigned picks the integer interpretation. Produces a
        // tightly-packed Float32x{count} buffer (bind at 0, arrayStride count*4).
        WGPUBuffer GetOrCreateFloatConvertedBuffer(MG_State::GLState::BufferObject& buffer, Uint32 offsetBytes,
                                                   Uint32 strideBytes, Uint32 componentBytes, Bool isSigned,
                                                   Uint32 count);
        WGPUBuffer GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer);
        WGPUBuffer GetDummyVertexBuffer();
        WGPUBuffer GetOrCreateBuffer(UnorderedMap<const MG_State::GLState::BufferObject*, WgpuBuffer>& cache,
                                     MG_State::GLState::BufferObject& buffer, WGPUBufferUsage usage);
        const WgpuTexture* GetOrCreateTexture(MG_State::GLState::ITextureObject& texture);
        // Cube-map counterpart of GetOrCreateTexture: uploads the 6 faces into a 6-layer
        // texture and returns a WgpuTexture whose `view` is a WGPUTextureViewDimension_Cube.
        const WgpuTexture* GetOrCreateCubeTexture(MG_State::GLState::ITextureObject& texture);
        // Uploads every valid 2D mip level stored in `mip` into `tex` via the queue.
        // Levels with no CPU pixels are left zero-initialized. bytesPerTexel comes from
        // the resolved WGPU format (RGBA8=4, RGBA16F=8, ...); it sizes the row pitch.
        // When swizzleSrcChannels != 0, a non-identity GL texture swizzle is baked into
        // the RGBA8 output: each texel becomes swizzle(baseExpand(src)) (see the .cpp).
        // swizzlePacked holds the 4 TextureSwizzleParam values (3 bits each).
        // `target`/`arrayLayer` select the source upload target and destination array layer:
        // defaults upload a plain 2D texture; a cube passes each face target and layer 0..5.
        void UploadTextureLevels(WGPUTexture tex, MG_State::GLState::TextureObjectMipmap& mip,
                                 Uint32 bytesPerTexel, Uint32 srcBytesPerTexel,
                                 Uint32 swizzleSrcChannels = 0, Uint32 swizzlePacked = 0,
                                 TextureUploadTarget target = TextureUploadTarget::Texture2D,
                                 Uint32 arrayLayer = 0);
        // Builds (or reuses) a WGPUSampler matching the GL sampler params
        // (glTexParameter / glBindSampler). Cached by the resolved param values.
        // comparison: emit a compare sampler (for sampler2DShadow / sampler_comparison).
        // forceNonFiltering: clamp to nearest + no compare (required when the paired
        // texture is depth sampled through a plain texture_2d<f32> = unfilterable-float).
        WGPUSampler GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler, Bool comparison = false,
                                       Bool forceNonFiltering = false);
        WGPUShaderModule MakeShaderModule(const char* wgsl);
        // Blit pipeline for the mipmap downsample, cached per color format (the render
        // target format must match the texture's).
        WGPURenderPipeline GetOrCreateMipPipeline(WGPUTextureFormat format);
        // Begins a Load render pass, binds the pipeline + vertex buffers for the
        // current program/VAO. Returns the pass (caller draws + ends) or nullptr.
        WGPURenderPassEncoder BeginDrawPass(MG_State::GLState::ProgramObject& program,
                                            const MG_State::GLState::VertexArrayObject& vao, GLenum mode,
                                            WGPUIndexFormat stripIndexFormat = WGPUIndexFormat_Undefined,
                                            Int64 vertexAccessEnd = -1);

        WGPUInstance m_instance = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUQueue m_queue = nullptr;
        WGPUSurface m_surface = nullptr;
        WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
        Uint32 m_width = 0;
        Uint32 m_height = 0;
        String m_canvasSelector;
        Platform::Handles m_platformHandles;

        // Caches (keyed by the MG_State object identity; minimal invalidation for now).
        UnorderedMap<const MG_State::GLState::ProgramObject*, WgpuProgram> m_programCache;
        // Keyed by a hash of (program, vertex layout, topology, depth/blend/cull/
        // color-mask state, target formats): WebGPU bakes all of that into the
        // pipeline, so each distinct render-state combination is its own variant.
        UnorderedMap<Uint64, WgpuPipeline> m_pipelineCache;
        UnorderedMap<const MG_State::GLState::BufferObject*, WgpuBuffer> m_vertexBufferCache;
        // Repacked (4-aligned-stride) vertex buffers, keyed by hash(buffer, origStride).
        UnorderedMap<Uint64, WgpuBuffer> m_repackedVertexBufferCache;
        UnorderedMap<const MG_State::GLState::BufferObject*, WgpuBuffer> m_indexBufferCache;
        WGPUBuffer m_dummyVertexBuffer = nullptr; // shared constant-zero VB (see GetDummyVertexBuffer)
        UnorderedMap<const MG_State::GLState::ITextureObject*, WgpuTexture> m_textureCache;
        // WGPUSamplers keyed by a hash of their resolved descriptor (GL sampler params).
        UnorderedMap<Uint64, WGPUSampler> m_samplerCache;
        // Mipmap-downsample blit: one shader module + a linear-clamp sampler, and a
        // render pipeline cached per color format (GenerateMipmaps).
        WGPUShaderModule m_mipShaderModule = nullptr;
        WGPUSampler m_mipSampler = nullptr;
        WGPUSampler m_blitNearestSampler = nullptr;
        UnorderedMap<Uint32, WGPURenderPipeline> m_mipPipelines;
        // Bind groups reference runtime resources (textures/UBO contents), so they are
        // rebuilt each draw and released at frame end.
        Vector<WGPUBindGroup> m_frameBindGroups;
        // Per-draw global-UBO buffers: each draw gets its own (queueWriteBuffer does not
        // interleave with the recorded passes, so a shared buffer would make every pass
        // in a frame read the last-written uniforms). Released at frame end.
        Vector<WGPUBuffer> m_frameUboBuffers;

        // Persistent offscreen color target: all rendering goes here, then Present
        // copies it to the acquired swapchain texture. ReadPixels copies from it.
        WGPUTexture m_offscreenTexture = nullptr;
        WGPUTextureView m_offscreenView = nullptr;
        WGPUTextureView m_offscreenSrgbView = nullptr;
        Uint32 m_offscreenWidth = 0;
        Uint32 m_offscreenHeight = 0;
        // Companion depth/stencil target for the default framebuffer.
        WGPUTexture m_depthTexture = nullptr;
        WGPUTextureView m_depthView = nullptr;
        // The GL default framebuffer carries stencil state too. Depth24PlusStencil8 is
        // core WebGPU and lets clears/tests match Vulkan instead of silently discarding
        // every stencil operation.
        static constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth24PlusStencil8;

        // Current draw target(s), resolved per Clear/draw from the bound draw FBO. For
        // the default FB slot 0 aliases the offscreen; for a user FBO the slots map to
        // its draw buffers (glDrawBuffers) -> color attachments, indexed by draw-buffer
        // slot == fragment output @location. A slot with a null view is a hole (draw
        // buffer None / unusable attachment). Pipelines (target formats + has-depth) are
        // keyed off these.
        static constexpr Uint kMaxColorTargets = 8; // matches FramebufferObject::MAX_DRAW_BUFFERS
        struct CurColorTarget {
            WGPUTextureView view = nullptr;
            WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        };
        CurColorTarget m_curColor[kMaxColorTargets];
        Uint m_curColorCount = 0; // number of resolved slots (holes included)
        WGPUTextureView m_curDepthView = nullptr; // null => target has no depth
        WGPUTextureFormat m_curDepthFormat = kDepthFormat; // format of m_curDepthView
        Uint32 m_curWidth = 0;
        Uint32 m_curHeight = 0;
        Bool m_curIsDefault = true;

        // Transient depth/stencil buffers for user FBO renderbuffer attachments.
        struct FboDepth {
            WGPUTexture texture = nullptr;
            WGPUTextureView view = nullptr;
            Uint32 width = 0;
            Uint32 height = 0;
        };
        UnorderedMap<const MG_State::GLState::FramebufferObject*, FboDepth> m_fboDepthCache;
        // Guards against reentrant readback while a JSPI suspension is in flight.
        Bool m_readbackInFlight = false;

        // Per-frame transient state (valid only between BeginFrameIfNeeded and Present)
        WGPUCommandEncoder m_encoder = nullptr;
        WGPUTexture m_frameTexture = nullptr;
        WGPUTextureView m_frameView = nullptr;
        Bool m_frameActive = false;
        // Bumped by every FlushFrame (submit). Resources whose lastUseSerial equals
        // the current value have been used by commands not yet submitted.
        Uint64 m_flushSerial = 1;
    };
} // namespace MobileGL::MG_Backend::DirectWebGPU
