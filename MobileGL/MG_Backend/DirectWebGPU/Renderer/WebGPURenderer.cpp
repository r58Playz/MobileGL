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
#include "MG_Util/ShaderTranspiler/WgslTranspiler.h"
#include <emscripten/html5.h>
#include <vector>

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
        for (auto& [k, pipe] : m_pipelineCache) { if (pipe) wgpuRenderPipelineRelease(pipe); }
        m_pipelineCache.clear();
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
        auto [ins, ok] = m_programCache.emplace(&program, prog);
        return &ins->second;
    }

    WGPURenderPipeline
    WebGPURenderer::GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                        const MG_State::GLState::VertexArrayObject& vao, GLenum mode) {
        if (auto it = m_pipelineCache.find(&program); it != m_pipelineCache.end()) {
            return it->second;
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
        if (pipeline) {
            m_pipelineCache.emplace(&program, pipeline);
        }
        return pipeline;
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

        WGPURenderPipeline pipeline = GetOrCreatePipeline(*programPtr, *vaoPtr, mode);
        if (!pipeline) {
            return;
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
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);

        Uint32 slot = 0;
        for (Uint i = 0; i < 16; ++i) {
            if (!vaoPtr->IsAttributeEnabled(i)) continue;
            const auto& a = vaoPtr->GetAttribute(i);
            if (ToVertexFormat(a.Type, a.Size) == WGPUVertexFormat_Force32 || !a.Buffer) continue;
            WGPUBuffer vb = GetOrCreateVertexBuffer(*a.Buffer);
            if (!vb) continue;
            wgpuRenderPassEncoderSetVertexBuffer(pass, slot, vb, static_cast<uint64_t>(a.Offset),
                                                 WGPU_WHOLE_SIZE);
            ++slot;
        }

        wgpuRenderPassEncoderDraw(pass, static_cast<Uint32>(count), 1, static_cast<Uint32>(first), 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
