// MobileGL - MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WebGPURenderer.h"
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/ProgramState/ShaderObject.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/TextureState/TextureUnit.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>
#include "MG_Util/ShaderTranspiler/WgslTranspiler.h"
#include <emscripten/html5.h>
#include <spirv_reflect.h>
#include <vector>
#include <cstring>

// Returns the device's preferred canvas format so the surface (and pipeline color
// targets, which share m_format) avoid an extra blit copy: 1 = rgba8unorm,
// 0 = bgra8unorm (the WebGPU-guaranteed canvas formats).
EM_JS(int, mobilegl_preferred_canvas_format, (), {
    try {
        return navigator["gpu"]["getPreferredCanvasFormat"]() === "rgba8unorm" ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

namespace MobileGL::MG_Backend::DirectWebGPU {
    namespace {
        // Minimal GL-type -> WGPU vertex format (M2b supports float attributes).
        template <typename DataTypeT>
        WGPUVertexFormat ToVertexFormat(DataTypeT type, int size) {
            if (type == DataTypeT::Float32) {
                switch (size) {
                case 1: return WGPUVertexFormat_Float32;
                case 2: return WGPUVertexFormat_Float32x2;
                case 3: return WGPUVertexFormat_Float32x3;
                case 4: return WGPUVertexFormat_Float32x4;
                default: break;
                }
            }
            return WGPUVertexFormat_Force32;
        }

        WGPUPrimitiveTopology ToTopology(GLenum mode) {
            switch (mode) {
            case GL_POINTS: return WGPUPrimitiveTopology_PointList;
            case GL_LINES: return WGPUPrimitiveTopology_LineList;
            case GL_LINE_STRIP: return WGPUPrimitiveTopology_LineStrip;
            case GL_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
            case GL_TRIANGLES:
            default: return WGPUPrimitiveTopology_TriangleList;
            }
        }

        WGPUIndexFormat ToIndexFormat(GLenum type) {
            switch (type) {
            case GL_UNSIGNED_SHORT: return WGPUIndexFormat_Uint16;
            case GL_UNSIGNED_INT: return WGPUIndexFormat_Uint32;
            default: return WGPUIndexFormat_Undefined; // GL_UNSIGNED_BYTE unsupported by WebGPU
            }
        }

        // Minimal GL internal format -> WGPU texture format (8-bit, 4-channel for now;
        // WebGPU has no 3-channel rgb8, so RGB textures must be expanded by the caller).
        WGPUTextureFormat ToTextureFormat(TextureInternalFormat fmt) {
            using F = TextureInternalFormat;
            switch (fmt) {
            case F::RGBA8: return WGPUTextureFormat_RGBA8Unorm;
            case F::SRGB8Alpha8: return WGPUTextureFormat_RGBA8UnormSrgb;
            default: return WGPUTextureFormat_Undefined;
            }
        }

        WGPUFilterMode ToWgpuFilter(SamplerFilterMode mode) {
            return mode == SamplerFilterMode::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
        }

        WGPUMipmapFilterMode ToWgpuMipmapFilter(SamplerMipmapMode mode) {
            return mode == SamplerMipmapMode::Linear ? WGPUMipmapFilterMode_Linear
                                                     : WGPUMipmapFilterMode_Nearest;
        }

        // WebGPU core has only ClampToEdge / Repeat / MirrorRepeat. ClampToBorder and
        // MirrorClampToEdge have no equivalent, so they degrade to ClampToEdge (border
        // color is a separate unsupported feature).
        WGPUAddressMode ToWgpuAddressMode(SamplerWrapMode mode) {
            switch (mode) {
            case SamplerWrapMode::Repeat: return WGPUAddressMode_Repeat;
            case SamplerWrapMode::MirroredRepeat: return WGPUAddressMode_MirrorRepeat;
            case SamplerWrapMode::ClampToEdge:
            case SamplerWrapMode::ClampToBorder:
            case SamplerWrapMode::MirrorClampToEdge:
            default: return WGPUAddressMode_ClampToEdge;
            }
        }
    } // namespace
} // namespace MobileGL::MG_Backend::DirectWebGPU

namespace MobileGL::MG_Backend::DirectWebGPU {
    WebGPURenderer::~WebGPURenderer() {
        Shutdown();
    }

    Bool WebGPURenderer::Initialize(const String& canvasSelector) {
        m_canvasSelector = canvasSelector;

        m_instance = wgpuCreateInstance(nullptr);
        if (!m_instance) {
            MGLOG_E("WebGPURenderer: wgpuCreateInstance failed");
            return false;
        }

        // The device is created asynchronously by JS at startup and handed to the
        // module; we retrieve the ready handle synchronously here.
        m_device = emscripten_webgpu_get_device();
        if (!m_device) {
            MGLOG_E("WebGPURenderer: no preinitialized WebGPU device "
                    "(set Module.preinitializedWebGPUDevice before creating the surface)");
            return false;
        }
        m_queue = wgpuDeviceGetQueue(m_device);

        // Match the canvas' preferred format (avoids an implicit copy at present).
        m_format = mobilegl_preferred_canvas_format() == 1 ? WGPUTextureFormat_RGBA8Unorm
                                                           : WGPUTextureFormat_BGRA8Unorm;

        // Create the canvas surface (emdawnwebgpu canvas selector source).
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSrc{};
        canvasSrc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvasSrc.selector = Wgpu::View(m_canvasSelector.c_str());
        WGPUSurfaceDescriptor surfaceDesc{};
        surfaceDesc.nextInChain = &canvasSrc.chain;
        m_surface = wgpuInstanceCreateSurface(m_instance, &surfaceDesc);
        if (!m_surface) {
            MGLOG_E("WebGPURenderer: wgpuInstanceCreateSurface failed for '%s'", m_canvasSelector.c_str());
            return false;
        }

        int w = 0, h = 0;
        emscripten_get_canvas_element_size(m_canvasSelector.c_str(), &w, &h);
        m_width = w > 0 ? static_cast<Uint32>(w) : 512u;
        m_height = h > 0 ? static_cast<Uint32>(h) : 512u;

        WGPUSurfaceConfiguration cfg{};
        cfg.device = m_device;
        cfg.format = m_format;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = m_width;
        cfg.height = m_height;
        cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
        cfg.presentMode = WGPUPresentMode_Fifo;
        wgpuSurfaceConfigure(m_surface, &cfg);

        MGLOG_I("WebGPURenderer initialized: canvas='%s' %ux%u", m_canvasSelector.c_str(), m_width, m_height);
        return true;
    }

    void WebGPURenderer::Shutdown() {
        EndFrame();
        for (auto& [k, buf] : m_vertexBufferCache) { if (buf) wgpuBufferRelease(buf); }
        m_vertexBufferCache.clear();
        for (auto& [k, buf] : m_indexBufferCache) { if (buf) wgpuBufferRelease(buf); }
        m_indexBufferCache.clear();
        for (auto& [k, p] : m_pipelineCache) {
            if (p.group0Layout) wgpuBindGroupLayoutRelease(p.group0Layout);
            if (p.uboBuffer) wgpuBufferRelease(p.uboBuffer);
            if (p.pipeline) wgpuRenderPipelineRelease(p.pipeline);
        }
        m_pipelineCache.clear();
        for (auto& [k, t] : m_textureCache) {
            if (t.view) wgpuTextureViewRelease(t.view);
            if (t.texture) wgpuTextureRelease(t.texture);
        }
        m_textureCache.clear();
        for (auto& [k, s] : m_samplerCache) { if (s) wgpuSamplerRelease(s); }
        m_samplerCache.clear();
        for (auto& [k, prog] : m_programCache) {
            if (prog.vertex) wgpuShaderModuleRelease(prog.vertex);
            if (prog.fragment) wgpuShaderModuleRelease(prog.fragment);
        }
        m_programCache.clear();
        if (m_surface) { wgpuSurfaceRelease(m_surface); m_surface = nullptr; }
        if (m_queue) { wgpuQueueRelease(m_queue); m_queue = nullptr; }
        // m_device is owned by JS (emscripten_webgpu_get_device); do not destroy it.
        m_device = nullptr;
        if (m_instance) { wgpuInstanceRelease(m_instance); m_instance = nullptr; }
    }

    Bool WebGPURenderer::AcquireSurfaceView() {
        WGPUSurfaceTexture st{};
        wgpuSurfaceGetCurrentTexture(m_surface, &st);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            MGLOG_E("WebGPURenderer: GetCurrentTexture status=%d", static_cast<int>(st.status));
            return false;
        }
        m_frameTexture = st.texture;
        m_frameView = wgpuTextureCreateView(m_frameTexture, nullptr);
        return m_frameView != nullptr;
    }

    void WebGPURenderer::BeginFrameIfNeeded() {
        if (m_frameActive) {
            return;
        }
        if (!AcquireSurfaceView()) {
            return;
        }
        m_encoder = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
        m_frameActive = true;
    }

    void WebGPURenderer::EndFrame() {
        // Bind groups were referenced by the just-submitted commands; safe to release now.
        for (WGPUBindGroup bg : m_frameBindGroups) { if (bg) wgpuBindGroupRelease(bg); }
        m_frameBindGroups.clear();
        if (m_frameView) { wgpuTextureViewRelease(m_frameView); m_frameView = nullptr; }
        if (m_frameTexture) { wgpuTextureRelease(m_frameTexture); m_frameTexture = nullptr; }
        if (m_encoder) { wgpuCommandEncoderRelease(m_encoder); m_encoder = nullptr; }
        m_frameActive = false;
    }

    void WebGPURenderer::Clear(GLbitfield mask) {
        if (!m_device || !MG_State::pGLContext) {
            return;
        }
        // M2a: default-framebuffer color clear only (depth/stencil added with the
        // draw path in M2b). WebGPU clears via a render pass loadOp.
        if ((mask & GL_COLOR_BUFFER_BIT) == 0) {
            return;
        }
        BeginFrameIfNeeded();
        if (!m_frameActive) {
            return;
        }

        const auto& c = MG_State::pGLContext->GetClearColor();
        WGPURenderPassColorAttachment color{};
        color.view = m_frameView;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {static_cast<double>(c[0]), static_cast<double>(c[1]),
                            static_cast<double>(c[2]), static_cast<double>(c[3])};

        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &rp);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    void WebGPURenderer::Present() {
        if (!m_frameActive) {
            // Nothing was recorded this frame; the browser keeps the last image.
            return;
        }
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(m_encoder, nullptr);
        wgpuQueueSubmit(m_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        EndFrame();
        // On the web the configured canvas surface is presented implicitly when
        // control returns to the browser event loop.
    }

    WGPUShaderModule WebGPURenderer::MakeShaderModule(const char* wgsl) {
        WGPUShaderSourceWGSL src{};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = Wgpu::View(wgsl);
        WGPUShaderModuleDescriptor desc{};
        desc.nextInChain = &src.chain;
        return wgpuDeviceCreateShaderModule(m_device, &desc);
    }

    const WebGPURenderer::WgpuProgram*
    WebGPURenderer::GetOrCreateProgram(MG_State::GLState::ProgramObject& program) {
        if (auto it = m_programCache.find(&program); it != m_programCache.end()) {
            return &it->second;
        }
        // SPIR-V is produced at link time, one blob per attached shader (parallel
        // to GetAttachedShaders). Transpile each stage to WGSL via tint.
        auto& spirv = program.GetGeneratedSpirv();
        auto& shaders = program.GetAttachedShaders();
        WgpuProgram prog;
        for (SizeT i = 0; i < shaders.size() && i < spirv.size(); ++i) {
            if (spirv[i].empty()) {
                continue;
            }
            std::vector<uint32_t> spv(spirv[i].begin(), spirv[i].end());
            auto wgsl = MG_Util::ShaderTranspiler::SpirvToWgsl(spv);
            if (!wgsl.success) {
                MGLOG_E("DirectWebGPU: SPIR-V -> WGSL failed: %s", wgsl.error.c_str());
                return nullptr;
            }
            WGPUShaderModule module = MakeShaderModule(wgsl.wgsl.c_str());
            switch (shaders[i]->GetShaderStage()) {
            case ShaderStage::Vertex: prog.vertex = module; break;
            case ShaderStage::Fragment: prog.fragment = module; break;
            default:
                wgpuShaderModuleRelease(module);
                break;
            }
        }
        if (!prog.vertex || !prog.fragment) {
            MGLOG_E("DirectWebGPU: program missing vertex or fragment WGSL module");
            if (prog.vertex) wgpuShaderModuleRelease(prog.vertex);
            if (prog.fragment) wgpuShaderModuleRelease(prog.fragment);
            return nullptr;
        }
        // Reflect resource bindings from the SPIR-V (SPIRV-Reflect) so they match the
        // @group(0)/@binding(N) that tint emits: the global UBO at its binding, and
        // each combined sampler2D at N (texture) / N+1 (sampler, tint's split).
        for (SizeT i = 0; i < spirv.size(); ++i) {
            if (spirv[i].empty()) continue;
            SpvReflectShaderModule mod;
            if (spvReflectCreateShaderModule(spirv[i].size() * sizeof(unsigned), spirv[i].data(), &mod) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                continue;
            }
            uint32_t count = 0;
            spvReflectEnumerateDescriptorBindings(&mod, &count, nullptr);
            std::vector<SpvReflectDescriptorBinding*> binds(count);
            spvReflectEnumerateDescriptorBindings(&mod, &count, binds.data());
            for (auto* b : binds) {
                if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    const char* tn = b->type_description ? b->type_description->type_name : nullptr;
                    if (prog.globalUboBinding < 0 && tn && String(tn).find("MGL_GLOBAL_UBO") != String::npos) {
                        prog.globalUboBinding = static_cast<Int>(b->binding);
                    }
                } else if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    Bool dup = false;
                    for (const auto& s : prog.samplers) {
                        if (s.textureBinding == b->binding) { dup = true; break; }
                    }
                    if (!dup) {
                        SamplerRef ref;
                        ref.textureBinding = b->binding;
                        ref.samplerBinding = b->binding + 1;
                        ref.name = b->name ? b->name : "";
                        prog.samplers.push_back(ref);
                    }
                }
            }
            spvReflectDestroyShaderModule(&mod);
        }
        if (prog.globalUboBinding >= 0) {
            prog.globalUboSize = program.GetUBOSize();
        }
        auto [ins, ok] = m_programCache.emplace(&program, prog);
        return &ins->second;
    }

    const WebGPURenderer::WgpuPipeline*
    WebGPURenderer::GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                        const MG_State::GLState::VertexArrayObject& vao, GLenum mode) {
        if (auto it = m_pipelineCache.find(&program); it != m_pipelineCache.end()) {
            return &it->second;
        }
        const WgpuProgram* prog = GetOrCreateProgram(program);
        if (!prog) {
            return nullptr;
        }

        // One vertex buffer layout per enabled+supported attribute, in ascending
        // attribute order (the draw path binds vertex buffers in the same order).
        constexpr Uint kMaxAttribs = 16;
        std::vector<WGPUVertexAttribute> attrs;
        attrs.reserve(kMaxAttribs);
        for (Uint i = 0; i < kMaxAttribs; ++i) {
            if (!vao.IsAttributeEnabled(i)) continue;
            const auto& a = vao.GetAttribute(i);
            if (ToVertexFormat(a.Type, a.Size) == WGPUVertexFormat_Force32) continue;
            WGPUVertexAttribute va{};
            va.format = ToVertexFormat(a.Type, a.Size);
            va.offset = 0;
            va.shaderLocation = i;
            attrs.push_back(va);
        }
        std::vector<WGPUVertexBufferLayout> layouts;
        layouts.reserve(attrs.size());
        SizeT ai = 0;
        for (Uint i = 0; i < kMaxAttribs; ++i) {
            if (!vao.IsAttributeEnabled(i)) continue;
            const auto& a = vao.GetAttribute(i);
            if (ToVertexFormat(a.Type, a.Size) == WGPUVertexFormat_Force32) continue;
            WGPUVertexBufferLayout layout{};
            layout.stepMode = WGPUVertexStepMode_Vertex;
            const uint64_t packed = static_cast<uint64_t>(4 * a.Size); // float components
            layout.arrayStride = a.Stride ? static_cast<uint64_t>(a.Stride) : packed;
            layout.attributeCount = 1;
            layout.attributes = &attrs[ai++];
            layouts.push_back(layout);
        }

        WGPUColorTargetState colorTarget{};
        colorTarget.format = m_format;
        colorTarget.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragment{};
        fragment.module = prog->fragment;
        fragment.entryPoint = Wgpu::View("main");
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        WGPURenderPipelineDescriptor desc{};
        desc.layout = nullptr; // auto layout: no bind groups in the minimal path
        desc.vertex.module = prog->vertex;
        desc.vertex.entryPoint = Wgpu::View("main");
        desc.vertex.bufferCount = layouts.size();
        desc.vertex.buffers = layouts.empty() ? nullptr : layouts.data();
        desc.primitive.topology = ToTopology(mode);
        desc.primitive.frontFace = WGPUFrontFace_CCW;
        desc.primitive.cullMode = WGPUCullMode_None;
        desc.multisample.count = 1;
        desc.multisample.mask = 0xFFFFFFFFu;
        desc.fragment = &fragment;

        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(m_device, &desc);
        if (!pipeline) {
            return nullptr;
        }
        WgpuPipeline entry;
        entry.pipeline = pipeline;
        // Keep the auto-generated group-0 layout for building per-draw bind groups,
        // and create the global-UBO backing buffer once.
        if (prog->HasResources()) {
            entry.group0Layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
            if (prog->globalUboBinding >= 0 && prog->globalUboSize > 0) {
                WGPUBufferDescriptor bd{};
                bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                bd.size = (static_cast<Uint64>(prog->globalUboSize) + 15u) & ~Uint64(15); // 16-byte aligned
                entry.uboBuffer = wgpuDeviceCreateBuffer(m_device, &bd);
            }
        }
        auto [ins, ok] = m_pipelineCache.emplace(&program, entry);
        return &ins->second;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer) {
        if (auto it = m_vertexBufferCache.find(&buffer); it != m_vertexBufferCache.end()) {
            return it->second; // M2b minimal: no update-on-change yet
        }
        const SizeT size = buffer.GetSize();
        const auto& data = buffer.GetDataReadOnly();
        if (size == 0 || !data) {
            return nullptr;
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = (size + 3u) & ~SizeT(3); // 4-byte aligned allocation
        WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!buf) {
            return nullptr;
        }
        wgpuQueueWriteBuffer(m_queue, buf, 0, data->data(), size & ~SizeT(3));
        m_vertexBufferCache.emplace(&buffer, buf);
        return buf;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer) {
        if (auto it = m_indexBufferCache.find(&buffer); it != m_indexBufferCache.end()) {
            return it->second;
        }
        const SizeT size = buffer.GetSize();
        const auto& data = buffer.GetDataReadOnly();
        if (size == 0 || !data) {
            return nullptr;
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        bd.size = (size + 3u) & ~SizeT(3);
        WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!buf) {
            return nullptr;
        }
        wgpuQueueWriteBuffer(m_queue, buf, 0, data->data(), size & ~SizeT(3));
        m_indexBufferCache.emplace(&buffer, buf);
        return buf;
    }

    const WebGPURenderer::WgpuTexture*
    WebGPURenderer::GetOrCreateTexture(MG_State::GLState::ITextureObject& texture) {
        const WGPUTextureFormat fmt = ToTextureFormat(texture.GetFormat());
        if (fmt == WGPUTextureFormat_Undefined) {
            MGLOG_E("DirectWebGPU: unsupported texture internal format for sampling");
            return nullptr;
        }
        auto* mip = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(&texture);
        if (!mip) {
            return nullptr;
        }
        const IntVec3 dim = texture.GetBaseSize();
        if (dim.x() <= 0 || dim.y() <= 0) {
            return nullptr;
        }
        const Uint32 w = static_cast<Uint32>(dim.x());
        const Uint32 h = static_cast<Uint32>(dim.y());
        const auto target = TextureUploadTarget::Texture2D;

        // Reuse the cached GPU texture unless its content is dirty (glTexImage2D /
        // glTexSubImage2D mark it so). A size/format change forces a recreate.
        auto it = m_textureCache.find(&texture);
        if (it != m_textureCache.end()) {
            WgpuTexture& cached = it->second;
            if (!mip->IsStorageDirty(target, 0)) {
                return &cached; // up to date
            }
            if (cached.width != w || cached.height != h || cached.format != fmt) {
                if (cached.view) wgpuTextureViewRelease(cached.view);
                if (cached.texture) wgpuTextureRelease(cached.texture);
                m_textureCache.erase(it);
            } else {
                if (UploadTextureLevel0(cached.texture, *mip, w, h)) {
                    mip->MarkStorageDirty(target, 0, false);
                }
                return &cached;
            }
        }

        WGPUTextureDescriptor td{};
        td.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {w, h, 1};
        td.format = fmt;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        WGPUTexture tex = wgpuDeviceCreateTexture(m_device, &td);
        if (!tex) {
            return nullptr;
        }
        if (!UploadTextureLevel0(tex, *mip, w, h)) {
            wgpuTextureRelease(tex);
            return nullptr;
        }
        mip->MarkStorageDirty(target, 0, false);

        WgpuTexture wt;
        wt.texture = tex;
        wt.view = wgpuTextureCreateView(tex, nullptr);
        wt.width = w;
        wt.height = h;
        wt.format = fmt;
        auto [ins, ok] = m_textureCache.emplace(&texture, wt);
        return &ins->second;
    }

    Bool WebGPURenderer::UploadTextureLevel0(WGPUTexture tex, MG_State::GLState::TextureObjectMipmap& mip,
                                             Uint32 w, Uint32 h) {
        const auto target = TextureUploadTarget::Texture2D;
        const SizeT byteSize = mip.GetMipmapByteSize(target, 0);
        void* pixels = mip.MapMipmapData(target, 0);
        if (!pixels || byteSize == 0) {
            return false;
        }
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = tex;
        WGPUTexelCopyBufferLayout dataLayout{};
        dataLayout.bytesPerRow = w * 4u; // RGBA8 (4 bytes/texel)
        dataLayout.rowsPerImage = h;
        WGPUExtent3D ext{w, h, 1};
        wgpuQueueWriteTexture(m_queue, &dst, pixels, byteSize, &dataLayout, &ext);
        return true;
    }

    WGPUSampler WebGPURenderer::GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler) {
        WGPUSamplerDescriptor sd{};
        sd.addressModeU = ToWgpuAddressMode(sampler.GetWrapS());
        sd.addressModeV = ToWgpuAddressMode(sampler.GetWrapT());
        sd.addressModeW = ToWgpuAddressMode(sampler.GetWrapR());
        sd.magFilter = ToWgpuFilter(sampler.GetMagFilter());
        sd.minFilter = ToWgpuFilter(sampler.GetMinFilter());
        sd.mipmapFilter = ToWgpuMipmapFilter(sampler.GetMipmapMode());
        // WebGPU requires 0 <= lodMinClamp <= lodMaxClamp; GL defaults (-1000..1000)
        // must be clamped. With no mipmaps yet this only affects validity.
        const Float maxLod = sampler.GetMipmapMode() == SamplerMipmapMode::None
                                 ? 0.0f
                                 : (sampler.GetMaxLod() < 0.0f ? 0.0f : sampler.GetMaxLod());
        const Float minLod = sampler.GetMinLod() < 0.0f ? 0.0f : sampler.GetMinLod();
        sd.lodMinClamp = minLod < maxLod ? minLod : maxLod;
        sd.lodMaxClamp = maxLod;
        sd.maxAnisotropy = 1;
        // compare-samplers are a depth-texture feature; not wired up yet (would need
        // tint to emit a sampler_comparison binding), so leave .compare unset.

        // Key the cache by the resolved WGPU descriptor fields (dedupes identical
        // samplers across textures, like the Vulkan backend's sampler cache).
        Uint64 key = 1469598103934665603ull; // FNV-1a
        auto mix = [&key](Uint32 v) {
            for (int i = 0; i < 4; ++i) {
                key ^= static_cast<Uint8>(v >> (i * 8));
                key *= 1099511628211ull;
            }
        };
        mix(static_cast<Uint32>(sd.addressModeU));
        mix(static_cast<Uint32>(sd.addressModeV));
        mix(static_cast<Uint32>(sd.addressModeW));
        mix(static_cast<Uint32>(sd.magFilter));
        mix(static_cast<Uint32>(sd.minFilter));
        mix(static_cast<Uint32>(sd.mipmapFilter));
        Uint32 lodBits;
        std::memcpy(&lodBits, &sd.lodMinClamp, 4); mix(lodBits);
        std::memcpy(&lodBits, &sd.lodMaxClamp, 4); mix(lodBits);

        if (auto it = m_samplerCache.find(key); it != m_samplerCache.end()) {
            return it->second;
        }
        WGPUSampler s = wgpuDeviceCreateSampler(m_device, &sd);
        m_samplerCache.emplace(key, s);
        return s;
    }

    WGPURenderPassEncoder WebGPURenderer::BeginDrawPass(MG_State::GLState::ProgramObject& program,
                                                        const MG_State::GLState::VertexArrayObject& vao,
                                                        GLenum mode) {
        const WgpuPipeline* p = GetOrCreatePipeline(program, vao, mode);
        if (!p) {
            return nullptr;
        }
        // Draw into a Load render pass so a prior glClear is preserved.
        WGPURenderPassColorAttachment color{};
        color.view = m_frameView;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, p->pipeline);

        // Build the group-0 bind group from current state: global UBO + sampled
        // textures. Rebuilt per draw (textures/UBO contents are runtime state).
        const WgpuProgram* prog = GetOrCreateProgram(program);
        if (prog && prog->HasResources() && p->group0Layout) {
            std::vector<WGPUBindGroupEntry> entries;
            if (prog->globalUboBinding >= 0 && p->uboBuffer) {
                const void* uboData = program.GetUBOData();
                const Uint sz = program.GetUBOSize();
                if (uboData && sz > 0) {
                    wgpuQueueWriteBuffer(m_queue, p->uboBuffer, 0, uboData, sz & ~Uint(3));
                }
                WGPUBindGroupEntry e{};
                e.binding = static_cast<Uint32>(prog->globalUboBinding);
                e.buffer = p->uboBuffer;
                e.size = (static_cast<Uint64>(prog->globalUboSize) + 15u) & ~Uint64(15);
                entries.push_back(e);
            }
            for (const auto& s : prog->samplers) {
                Int unit = 0;
                const Int loc = program.GetUniformLocation(s.name);
                if (loc >= 0) {
                    const Int u = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(loc));
                    if (u >= 0) unit = u;
                }
                auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                auto texObj = textureUnit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
                if (!texObj) continue;
                const WgpuTexture* wt = GetOrCreateTexture(*texObj);
                if (!wt) continue;
                // Effective sampler: a bound GL sampler object (glBindSampler) overrides
                // the texture object's own glTexParameter-set params.
                const auto& samplerOverride = textureUnit.GetSamplerObject();
                const auto& effectiveSampler = samplerOverride ? samplerOverride : texObj->GetSamplerObject();
                WGPUSampler sampler = effectiveSampler ? GetOrCreateSampler(*effectiveSampler) : nullptr;
                if (!sampler) continue;
                WGPUBindGroupEntry te{};
                te.binding = s.textureBinding;
                te.textureView = wt->view;
                entries.push_back(te);
                WGPUBindGroupEntry se{};
                se.binding = s.samplerBinding;
                se.sampler = sampler;
                entries.push_back(se);
            }
            if (!entries.empty()) {
                WGPUBindGroupDescriptor bgd{};
                bgd.layout = p->group0Layout;
                bgd.entryCount = entries.size();
                bgd.entries = entries.data();
                WGPUBindGroup bg = wgpuDeviceCreateBindGroup(m_device, &bgd);
                m_frameBindGroups.push_back(bg);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
            }
        }

        Uint32 slot = 0;
        for (Uint i = 0; i < 16; ++i) {
            if (!vao.IsAttributeEnabled(i)) continue;
            const auto& a = vao.GetAttribute(i);
            if (ToVertexFormat(a.Type, a.Size) == WGPUVertexFormat_Force32 || !a.Buffer) continue;
            WGPUBuffer vb = GetOrCreateVertexBuffer(*a.Buffer);
            if (!vb) continue;
            wgpuRenderPassEncoderSetVertexBuffer(pass, slot, vb, static_cast<uint64_t>(a.Offset),
                                                 WGPU_WHOLE_SIZE);
            ++slot;
        }
        return pass;
    }

    void WebGPURenderer::DrawArrays(GLenum mode, GLint first, GLsizei count) {
        if (!m_device || !MG_State::pGLContext || count <= 0) {
            return;
        }
        const auto& programPtr = MG_State::pGLContext->GetCurrentProgram();
        const auto& vaoPtr = MG_State::pGLContext->GetBoundVertexArray();
        if (!programPtr || !vaoPtr) {
            return;
        }
        BeginFrameIfNeeded();
        if (!m_frameActive) {
            return;
        }
        WGPURenderPassEncoder pass = BeginDrawPass(*programPtr, *vaoPtr, mode);
        if (!pass) {
            return;
        }
        wgpuRenderPassEncoderDraw(pass, static_cast<Uint32>(count), 1, static_cast<Uint32>(first), 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    void WebGPURenderer::DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        if (!m_device || !MG_State::pGLContext || count <= 0) {
            return;
        }
        const WGPUIndexFormat indexFormat = ToIndexFormat(type);
        if (indexFormat == WGPUIndexFormat_Undefined) {
            MGLOG_E("DirectWebGPU: unsupported index type 0x%x (GL_UNSIGNED_BYTE not in WebGPU)", type);
            return;
        }
        const auto& programPtr = MG_State::pGLContext->GetCurrentProgram();
        const auto& vaoPtr = MG_State::pGLContext->GetBoundVertexArray();
        if (!programPtr || !vaoPtr) {
            return;
        }
        const auto& iboPtr = vaoPtr->GetIndexBufferBindingSlot().GetBoundObject();
        if (!iboPtr) {
            MGLOG_E("DirectWebGPU: DrawElements with no element array buffer bound");
            return;
        }
        BeginFrameIfNeeded();
        if (!m_frameActive) {
            return;
        }
        WGPUBuffer indexBuffer = GetOrCreateIndexBuffer(*iboPtr);
        if (!indexBuffer) {
            return;
        }
        WGPURenderPassEncoder pass = BeginDrawPass(*programPtr, *vaoPtr, mode);
        if (!pass) {
            return;
        }
        // `indices` is a byte offset into the bound element array buffer.
        const uint64_t byteOffset = reinterpret_cast<uintptr_t>(indices);
        wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer, indexFormat, byteOffset, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(pass, static_cast<Uint32>(count), 1, 0, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
