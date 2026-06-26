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
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include "MG_Util/ShaderTranspiler/WgslTranspiler.h"
#include <emscripten/html5.h>
#include <spirv_reflect.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>

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

// JSPI suspend primitive (defined in lib_mobilegl_webgpu.js). mobilegl_jspi_wait is
// wrapped in WebAssembly.Suspending: calling it suspends the whole wasm stack (which
// must have been entered via WebAssembly.promising) until mobilegl_jspi_signal()
// resolves the pending promise from a WebGPU completion callback.
extern "C" void mobilegl_jspi_wait();
extern "C" void mobilegl_jspi_signal();

namespace MobileGL::MG_Backend::DirectWebGPU {
    namespace {
        // Component byte size of a vertex attribute element.
        Uint32 ComponentByteSize(DataType type) {
            switch (type) {
            case DataType::Int8:
            case DataType::Uint8: return 1;
            case DataType::Int16:
            case DataType::Uint16:
            case DataType::Float16: return 2;
            default: return 4; // Int32/Uint32/Float32/Fixed32
            }
        }

        // GL vertex attribute -> WGPU vertex format. WebGPU's 8/16-bit formats only come
        // in x2/x4, so size 1/3 of those types is unsupported (returns the Force32
        // sentinel and the attribute is skipped). `normalized` picks Unorm/Snorm;
        // `isInteger` (glVertexAttribIPointer) picks Uint/Sint; otherwise float.
        WGPUVertexFormat ToVertexFormat(DataType type, int size, Bool normalized, Bool isInteger) {
            const Bool n = normalized;
            switch (type) {
            case DataType::Float32:
                switch (size) {
                case 1: return WGPUVertexFormat_Float32;
                case 2: return WGPUVertexFormat_Float32x2;
                case 3: return WGPUVertexFormat_Float32x3;
                case 4: return WGPUVertexFormat_Float32x4;
                }
                break;
            case DataType::Float16:
                if (size == 2) return WGPUVertexFormat_Float16x2;
                if (size == 4) return WGPUVertexFormat_Float16x4;
                break;
            case DataType::Uint8:
                if (size == 2) return n ? WGPUVertexFormat_Unorm8x2 : WGPUVertexFormat_Uint8x2;
                if (size == 4) return n ? WGPUVertexFormat_Unorm8x4 : WGPUVertexFormat_Uint8x4;
                break;
            case DataType::Int8:
                if (size == 2) return n ? WGPUVertexFormat_Snorm8x2 : WGPUVertexFormat_Sint8x2;
                if (size == 4) return n ? WGPUVertexFormat_Snorm8x4 : WGPUVertexFormat_Sint8x4;
                break;
            case DataType::Uint16:
                if (size == 2) return n ? WGPUVertexFormat_Unorm16x2 : WGPUVertexFormat_Uint16x2;
                if (size == 4) return n ? WGPUVertexFormat_Unorm16x4 : WGPUVertexFormat_Uint16x4;
                break;
            case DataType::Int16:
                if (size == 2) return n ? WGPUVertexFormat_Snorm16x2 : WGPUVertexFormat_Sint16x2;
                if (size == 4) return n ? WGPUVertexFormat_Snorm16x4 : WGPUVertexFormat_Sint16x4;
                break;
            case DataType::Uint32:
                switch (size) {
                case 1: return WGPUVertexFormat_Uint32;
                case 2: return WGPUVertexFormat_Uint32x2;
                case 3: return WGPUVertexFormat_Uint32x3;
                case 4: return WGPUVertexFormat_Uint32x4;
                }
                break;
            case DataType::Int32:
                switch (size) {
                case 1: return WGPUVertexFormat_Sint32;
                case 2: return WGPUVertexFormat_Sint32x2;
                case 3: return WGPUVertexFormat_Sint32x3;
                case 4: return WGPUVertexFormat_Sint32x4;
                }
                break;
            default: break;
            }
            (void)isInteger;
            return WGPUVertexFormat_Force32; // unsupported (type/size combination)
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

        WGPUCompareFunction ToCompareFunc(DepthTestFunc f) {
            switch (f) {
            case DepthTestFunc::Never: return WGPUCompareFunction_Never;
            case DepthTestFunc::Less: return WGPUCompareFunction_Less;
            case DepthTestFunc::Equal: return WGPUCompareFunction_Equal;
            case DepthTestFunc::LessEqual: return WGPUCompareFunction_LessEqual;
            case DepthTestFunc::Greater: return WGPUCompareFunction_Greater;
            case DepthTestFunc::NotEqual: return WGPUCompareFunction_NotEqual;
            case DepthTestFunc::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
            case DepthTestFunc::Always:
            default: return WGPUCompareFunction_Always;
            }
        }

        WGPUBlendFactor ToBlendFactor(BlendFactor f) {
            switch (f) {
            case BlendFactor::Zero: return WGPUBlendFactor_Zero;
            case BlendFactor::One: return WGPUBlendFactor_One;
            case BlendFactor::SrcColor: return WGPUBlendFactor_Src;
            case BlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
            case BlendFactor::DstColor: return WGPUBlendFactor_Dst;
            case BlendFactor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
            case BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
            case BlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
            case BlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
            case BlendFactor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
            case BlendFactor::ConstantColor: return WGPUBlendFactor_Constant;
            case BlendFactor::OneMinusConstantColor: return WGPUBlendFactor_OneMinusConstant;
            // WebGPU has no separate constant-alpha factor; constant covers both.
            case BlendFactor::ConstantAlpha: return WGPUBlendFactor_Constant;
            case BlendFactor::OneMinusConstantAlpha: return WGPUBlendFactor_OneMinusConstant;
            default: return WGPUBlendFactor_One;
            }
        }

        WGPUBlendOperation ToBlendOp(BlendEquation e) {
            switch (e) {
            case BlendEquation::Add: return WGPUBlendOperation_Add;
            case BlendEquation::Subtract: return WGPUBlendOperation_Subtract;
            case BlendEquation::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
            case BlendEquation::Min: return WGPUBlendOperation_Min;
            case BlendEquation::Max: return WGPUBlendOperation_Max;
            default: return WGPUBlendOperation_Add;
            }
        }

        // GL determines front-facing in window space (y-up); WebGPU does so in the
        // framebuffer (y-down), so the winding is inverted (matching DirectVulkan).
        WGPUFrontFace ToFrontFace(FrontFaceMode m) {
            return m == FrontFaceMode::Clockwise ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
        }

        WGPUCullMode ToCullMode(CullFaceMode m) {
            switch (m) {
            case CullFaceMode::Front: return WGPUCullMode_Front;
            case CullFaceMode::Back: return WGPUCullMode_Back;
            // FrontAndBack would cull everything; WebGPU can't express it in cullMode,
            // so leave it to a (future) rasterizer-discard path — treat as Back here.
            case CullFaceMode::FrontAndBack:
            default: return WGPUCullMode_Back;
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
        // CopyDst so Present can copy the offscreen target onto the swapchain texture.
        cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
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
        for (auto& [k, b] : m_vertexBufferCache) { if (b.buffer) wgpuBufferRelease(b.buffer); }
        m_vertexBufferCache.clear();
        for (auto& [k, b] : m_indexBufferCache) { if (b.buffer) wgpuBufferRelease(b.buffer); }
        m_indexBufferCache.clear();
        for (auto& [k, p] : m_pipelineCache) {
            if (p.group0Layout) wgpuBindGroupLayoutRelease(p.group0Layout);
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
        if (m_offscreenView) { wgpuTextureViewRelease(m_offscreenView); m_offscreenView = nullptr; }
        if (m_offscreenTexture) { wgpuTextureRelease(m_offscreenTexture); m_offscreenTexture = nullptr; }
        if (m_depthView) { wgpuTextureViewRelease(m_depthView); m_depthView = nullptr; }
        if (m_depthTexture) { wgpuTextureRelease(m_depthTexture); m_depthTexture = nullptr; }
        for (auto& [k, fd] : m_fboDepthCache) {
            if (fd.view) wgpuTextureViewRelease(fd.view);
            if (fd.texture) wgpuTextureRelease(fd.texture);
        }
        m_fboDepthCache.clear();
        m_offscreenWidth = m_offscreenHeight = 0;
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

    void WebGPURenderer::EnsureOffscreenTarget() {
        if (m_offscreenTexture && m_offscreenWidth == m_width && m_offscreenHeight == m_height) {
            return;
        }
        if (m_offscreenView) { wgpuTextureViewRelease(m_offscreenView); m_offscreenView = nullptr; }
        if (m_offscreenTexture) { wgpuTextureRelease(m_offscreenTexture); m_offscreenTexture = nullptr; }
        if (m_depthView) { wgpuTextureViewRelease(m_depthView); m_depthView = nullptr; }
        if (m_depthTexture) { wgpuTextureRelease(m_depthTexture); m_depthTexture = nullptr; }
        WGPUTextureDescriptor td{};
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                   WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {m_width, m_height, 1};
        td.format = m_format;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        m_offscreenTexture = wgpuDeviceCreateTexture(m_device, &td);
        m_offscreenView = m_offscreenTexture ? wgpuTextureCreateView(m_offscreenTexture, nullptr) : nullptr;

        WGPUTextureDescriptor dd{};
        dd.usage = WGPUTextureUsage_RenderAttachment;
        dd.dimension = WGPUTextureDimension_2D;
        dd.size = {m_width, m_height, 1};
        dd.format = kDepthFormat;
        dd.mipLevelCount = 1;
        dd.sampleCount = 1;
        m_depthTexture = wgpuDeviceCreateTexture(m_device, &dd);
        m_depthView = m_depthTexture ? wgpuTextureCreateView(m_depthTexture, nullptr) : nullptr;

        m_offscreenWidth = m_width;
        m_offscreenHeight = m_height;
    }

    WGPUTextureView WebGPURenderer::GetOrCreateFboDepth(const MG_State::GLState::FramebufferObject& fbo,
                                                        Uint32 w, Uint32 h) {
        auto it = m_fboDepthCache.find(&fbo);
        if (it != m_fboDepthCache.end()) {
            if (it->second.width == w && it->second.height == h) {
                return it->second.view;
            }
            if (it->second.view) wgpuTextureViewRelease(it->second.view);
            if (it->second.texture) wgpuTextureRelease(it->second.texture);
            m_fboDepthCache.erase(it);
        }
        WGPUTextureDescriptor dd{};
        dd.usage = WGPUTextureUsage_RenderAttachment;
        dd.dimension = WGPUTextureDimension_2D;
        dd.size = {w, h, 1};
        dd.format = kDepthFormat;
        dd.mipLevelCount = 1;
        dd.sampleCount = 1;
        FboDepth fd;
        fd.texture = wgpuDeviceCreateTexture(m_device, &dd);
        fd.view = fd.texture ? wgpuTextureCreateView(fd.texture, nullptr) : nullptr;
        fd.width = w;
        fd.height = h;
        auto [ins, ok] = m_fboDepthCache.emplace(&fbo, fd);
        return ins->second.view;
    }

    Bool WebGPURenderer::ResolveDrawTarget() {
        auto* gl = MG_State::pGLContext.get();
        const auto& fbo = gl->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (!fbo || fbo->IsDefaultFramebuffer()) {
            EnsureOffscreenTarget();
            if (!m_offscreenView) {
                return false;
            }
            m_curColorView = m_offscreenView;
            m_curDepthView = m_depthView;
            m_curColorFormat = m_format;
            m_curWidth = m_width;
            m_curHeight = m_height;
            m_curIsDefault = true;
            return true;
        }
        // User FBO: single texture color attachment 0 (renderbuffer/MRT not supported yet).
        const auto& color0 = fbo->GetAttachment(FramebufferAttachmentType::Color0);
        if (!color0.IsValid() || color0.IsEmpty() || !color0.IsTexture()) {
            MGLOG_E("DirectWebGPU: FBO %u lacks a texture color attachment 0 (renderbuffer/MRT unsupported)",
                    fbo->GetExternalIndex());
            return false;
        }
        const WgpuTexture* wt = GetOrCreateTexture(*color0.GetTexture());
        if (!wt || !wt->view) {
            return false;
        }
        m_curColorView = wt->view;
        m_curColorFormat = wt->format;
        m_curWidth = wt->width;
        m_curHeight = wt->height;
        m_curIsDefault = false;
        // Depth/stencil attachment -> a transient depth buffer (contents not sampled yet).
        const auto& depthAtt = fbo->GetAttachment(FramebufferAttachmentType::Depth);
        m_curDepthView = (depthAtt.IsValid() && !depthAtt.IsEmpty())
                             ? GetOrCreateFboDepth(*fbo, wt->width, wt->height)
                             : nullptr;
        return true;
    }

    Bool WebGPURenderer::ResolveColorTexture(FramebufferTarget target, WGPUTexture& outTex, Uint32& outW,
                                             Uint32& outH, WGPUTextureFormat& outFmt) {
        auto* gl = MG_State::pGLContext.get();
        const auto& fbo = gl->GetFramebufferBindingSlot(target).GetBoundObject();
        if (!fbo || fbo->IsDefaultFramebuffer()) {
            EnsureOffscreenTarget();
            if (!m_offscreenTexture) return false;
            outTex = m_offscreenTexture;
            outW = m_width;
            outH = m_height;
            outFmt = m_format;
            return true;
        }
        const auto& color0 = fbo->GetAttachment(FramebufferAttachmentType::Color0);
        if (!color0.IsValid() || color0.IsEmpty() || !color0.IsTexture()) return false;
        const WgpuTexture* wt = GetOrCreateTexture(*color0.GetTexture());
        if (!wt || !wt->texture) return false;
        outTex = wt->texture;
        outW = wt->width;
        outH = wt->height;
        outFmt = wt->format;
        return true;
    }

    void WebGPURenderer::BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                         GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                                         GLenum filter) {
        (void)filter;
        if (!m_device || (mask & GL_COLOR_BUFFER_BIT) == 0) {
            return; // depth/stencil blits unsupported for now
        }
        WGPUTexture srcTex = nullptr, dstTex = nullptr;
        Uint32 srcW = 0, srcH = 0, dstW = 0, dstH = 0;
        WGPUTextureFormat srcFmt = WGPUTextureFormat_Undefined, dstFmt = WGPUTextureFormat_Undefined;
        if (!ResolveColorTexture(FramebufferTarget::Read, srcTex, srcW, srcH, srcFmt) ||
            !ResolveColorTexture(FramebufferTarget::Draw, dstTex, dstW, dstH, dstFmt)) {
            return;
        }
        const Int32 w = std::abs(srcX1 - srcX0), h = std::abs(srcY1 - srcY0);
        const Int32 dw = std::abs(dstX1 - dstX0), dh = std::abs(dstY1 - dstY0);
        if (w != dw || h != dh) {
            MGLOG_E("DirectWebGPU: scaling glBlitFramebuffer unsupported (src %dx%d -> dst %dx%d)", w, h, dw,
                    dh);
            return;
        }
        if (srcFmt != dstFmt || srcTex == dstTex) {
            MGLOG_E("DirectWebGPU: glBlitFramebuffer needs matching formats and distinct src/dst");
            return;
        }
        BeginFrameIfNeeded();
        if (!m_frameActive) {
            return;
        }
        // GL framebuffer coords are bottom-left origin; flip to top-left texel space.
        const Int32 sx = std::min(srcX0, srcX1), sy = static_cast<Int32>(srcH) - std::max(srcY0, srcY1);
        const Int32 dx = std::min(dstX0, dstX1), dy = static_cast<Int32>(dstH) - std::max(dstY0, dstY1);
        WGPUTexelCopyTextureInfo src{};
        src.texture = srcTex;
        src.origin = {static_cast<Uint32>(sx < 0 ? 0 : sx), static_cast<Uint32>(sy < 0 ? 0 : sy), 0};
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = dstTex;
        dst.origin = {static_cast<Uint32>(dx < 0 ? 0 : dx), static_cast<Uint32>(dy < 0 ? 0 : dy), 0};
        WGPUExtent3D ext{static_cast<Uint32>(w), static_cast<Uint32>(h), 1};
        wgpuCommandEncoderCopyTextureToTexture(m_encoder, &src, &dst, &ext);
    }

    void WebGPURenderer::BeginFrameIfNeeded() {
        if (m_frameActive) {
            return;
        }
        EnsureOffscreenTarget();
        if (!m_offscreenView) {
            return;
        }
        m_encoder = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
        m_frameActive = true;
    }

    void WebGPURenderer::FlushFrame() {
        if (!m_frameActive || !m_encoder) {
            return;
        }
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(m_encoder, nullptr);
        wgpuQueueSubmit(m_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(m_encoder);
        // Per-frame bind groups + UBO buffers were referenced by the just-submitted
        // commands; safe to release now.
        for (WGPUBindGroup bg : m_frameBindGroups) { if (bg) wgpuBindGroupRelease(bg); }
        m_frameBindGroups.clear();
        for (WGPUBuffer ub : m_frameUboBuffers) { if (ub) wgpuBufferRelease(ub); }
        m_frameUboBuffers.clear();
        // Keep the frame active: open a fresh encoder so subsequent draws continue
        // accumulating into the (persistent) offscreen target.
        m_encoder = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
    }

    void WebGPURenderer::EndFrame() {
        // Bind groups + UBO buffers were referenced by the just-submitted commands.
        for (WGPUBindGroup bg : m_frameBindGroups) { if (bg) wgpuBindGroupRelease(bg); }
        m_frameBindGroups.clear();
        for (WGPUBuffer ub : m_frameUboBuffers) { if (ub) wgpuBufferRelease(ub); }
        m_frameUboBuffers.clear();
        if (m_frameView) { wgpuTextureViewRelease(m_frameView); m_frameView = nullptr; }
        if (m_frameTexture) { wgpuTextureRelease(m_frameTexture); m_frameTexture = nullptr; }
        if (m_encoder) { wgpuCommandEncoderRelease(m_encoder); m_encoder = nullptr; }
        m_frameActive = false;
    }

    void WebGPURenderer::Clear(GLbitfield mask) {
        if (!m_device || !MG_State::pGLContext) {
            return;
        }
        const Bool clearColor = (mask & GL_COLOR_BUFFER_BIT) != 0;
        const Bool clearDepth = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        if (!clearColor && !clearDepth) {
            return; // stencil-only clear unsupported for now
        }
        BeginFrameIfNeeded();
        if (!m_frameActive) {
            return;
        }
        auto* gl = MG_State::pGLContext.get();
        if (!ResolveDrawTarget()) {
            return;
        }
        // A depth clear only applies if the current target actually has depth.
        const Bool doDepth = clearDepth && m_curDepthView != nullptr;

        WGPURenderPassColorAttachment color{};
        if (clearColor) {
            const auto& c = gl->GetClearColor();
            color.view = m_curColorView;
            color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = {static_cast<double>(c[0]), static_cast<double>(c[1]),
                                static_cast<double>(c[2]), static_cast<double>(c[3])};
        }

        WGPURenderPassDepthStencilAttachment depth{};
        if (doDepth) {
            depth.view = m_curDepthView;
            depth.depthLoadOp = WGPULoadOp_Clear;
            depth.depthStoreOp = WGPUStoreOp_Store;
            depth.depthClearValue = gl->GetClearDepth();
        }

        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = clearColor ? 1 : 0;
        rp.colorAttachments = clearColor ? &color : nullptr;
        rp.depthStencilAttachment = doDepth ? &depth : nullptr;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &rp);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    void WebGPURenderer::Present() {
        if (!m_frameActive) {
            // Nothing was recorded this frame; the browser keeps the last image.
            return;
        }
        // Copy the offscreen target onto the acquired swapchain texture, then submit.
        if (AcquireSurfaceView()) {
            WGPUTexelCopyTextureInfo src{};
            src.texture = m_offscreenTexture;
            WGPUTexelCopyTextureInfo dst{};
            dst.texture = m_frameTexture;
            WGPUExtent3D ext{m_width, m_height, 1};
            wgpuCommandEncoderCopyTextureToTexture(m_encoder, &src, &dst, &ext);
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

    Uint64 WebGPURenderer::ComputePipelineKey(const MG_State::GLState::ProgramObject& program,
                                              const MG_State::GLState::VertexArrayObject& vao,
                                              GLenum mode) const {
        Uint64 key = 1469598103934665603ull; // FNV-1a
        auto mix = [&key](Uint64 v) {
            for (int i = 0; i < 8; ++i) {
                key ^= static_cast<Uint8>(v >> (i * 8));
                key *= 1099511628211ull;
            }
        };
        mix(static_cast<Uint64>(reinterpret_cast<SizeT>(&program)));
        mix(static_cast<Uint64>(mode));
        // Vertex layout (per enabled attribute: index, format, stride, instance divisor>0).
        for (Uint i = 0; i < 16; ++i) {
            if (!vao.IsAttributeEnabled(i)) continue;
            const auto& a = vao.GetAttribute(i);
            mix((static_cast<Uint64>(i) << 40) ^ (static_cast<Uint64>(a.Stride) << 8) ^
                (a.Divisor > 0 ? (Uint64(1) << 60) : 0) ^ static_cast<Uint64>(ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger)));
        }
        // Render state WebGPU bakes into the pipeline.
        auto* gl = MG_State::pGLContext.get();
        const Bool depthTest = gl->IsCapabilityEnabled(CapabilityInput::DepthTest);
        mix(depthTest ? 1 : 0);
        mix(depthTest && gl->GetDepthMask() ? 1 : 0);
        mix(static_cast<Uint64>(gl->GetDepthFunc()));
        const Bool cull = gl->IsCapabilityEnabled(CapabilityInput::CullFace);
        mix(cull ? 1 : 0);
        mix(static_cast<Uint64>(gl->GetCullFaceMode()));
        mix(static_cast<Uint64>(gl->GetFrontFaceMode()));
        const auto cmask = gl->GetColorMask();
        mix((cmask.r() ? 1u : 0u) | (cmask.g() ? 2u : 0u) | (cmask.b() ? 4u : 0u) | (cmask.a() ? 8u : 0u));
        const Bool blend = gl->IsCapabilityEnabled(CapabilityInput::Blend);
        mix(blend ? 1 : 0);
        if (blend) {
            BlendFactor sR, dR, sA, dA;
            gl->GetBlendFunc(sR, dR, sA, dA);
            BlendEquation eC, eA;
            gl->GetBlendEquation(eC, eA);
            mix(static_cast<Uint64>(sR) | (static_cast<Uint64>(dR) << 8) | (static_cast<Uint64>(sA) << 16) |
                (static_cast<Uint64>(dA) << 24) | (static_cast<Uint64>(eC) << 32) |
                (static_cast<Uint64>(eA) << 40));
        }
        // Current draw target: color format + whether it has a depth attachment (both
        // baked into the pipeline by WebGPU).
        mix(static_cast<Uint64>(m_curColorFormat));
        mix(m_curDepthView != nullptr ? 1 : 0);
        return key;
    }

    const WebGPURenderer::WgpuPipeline*
    WebGPURenderer::GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                        const MG_State::GLState::VertexArrayObject& vao, GLenum mode) {
        const Uint64 key = ComputePipelineKey(program, vao, mode);
        if (auto it = m_pipelineCache.find(key); it != m_pipelineCache.end()) {
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
            if (ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger) == WGPUVertexFormat_Force32) continue;
            WGPUVertexAttribute va{};
            va.format = ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger);
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
            if (ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger) == WGPUVertexFormat_Force32) continue;
            WGPUVertexBufferLayout layout{};
            // Divisor>0 -> per-instance attribute (glVertexAttribDivisor); 0 -> per-vertex.
            layout.stepMode = a.Divisor > 0 ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
            // Tightly-packed stride from the actual component size; WebGPU requires the
            // arrayStride to be a multiple of 4, so round up.
            uint64_t packed = static_cast<uint64_t>(ComponentByteSize(a.Type)) * a.Size;
            packed = (packed + 3u) & ~uint64_t(3);
            layout.arrayStride = a.Stride ? static_cast<uint64_t>(a.Stride) : packed;
            layout.attributeCount = 1;
            layout.attributes = &attrs[ai++];
            layouts.push_back(layout);
        }

        auto* gl = MG_State::pGLContext.get();

        // Color target: write mask + (optional) blend, from GL state.
        const auto cmask = gl->GetColorMask();
        WGPUColorTargetState colorTarget{};
        colorTarget.format = m_curColorFormat;
        colorTarget.writeMask = (cmask.r() ? WGPUColorWriteMask_Red : 0u) |
                                (cmask.g() ? WGPUColorWriteMask_Green : 0u) |
                                (cmask.b() ? WGPUColorWriteMask_Blue : 0u) |
                                (cmask.a() ? WGPUColorWriteMask_Alpha : 0u);
        WGPUBlendState blend{};
        if (gl->IsCapabilityEnabled(CapabilityInput::Blend)) {
            BlendFactor sR, dR, sA, dA;
            gl->GetBlendFunc(sR, dR, sA, dA);
            BlendEquation eC, eA;
            gl->GetBlendEquation(eC, eA);
            blend.color = {ToBlendOp(eC), ToBlendFactor(sR), ToBlendFactor(dR)};
            blend.alpha = {ToBlendOp(eA), ToBlendFactor(sA), ToBlendFactor(dA)};
            colorTarget.blend = &blend;
        }

        WGPUFragmentState fragment{};
        fragment.module = prog->fragment;
        fragment.entryPoint = Wgpu::View("main");
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        // Depth-stencil: declared only if the current target has a depth attachment.
        // depthWrite only when the test is on (GL doesn't write depth with the test off).
        const Bool hasDepth = (m_curDepthView != nullptr);
        const Bool depthTest = gl->IsCapabilityEnabled(CapabilityInput::DepthTest);
        WGPUDepthStencilState depthState{};
        depthState.format = kDepthFormat;
        depthState.depthWriteEnabled = (depthTest && gl->GetDepthMask())
                                           ? WGPUOptionalBool_True
                                           : WGPUOptionalBool_False;
        depthState.depthCompare = depthTest ? ToCompareFunc(gl->GetDepthFunc()) : WGPUCompareFunction_Always;
        depthState.stencilFront.compare = WGPUCompareFunction_Always;
        depthState.stencilBack.compare = WGPUCompareFunction_Always;

        WGPURenderPipelineDescriptor desc{};
        desc.layout = nullptr; // auto layout; bind groups built from the pipeline's layout
        desc.vertex.module = prog->vertex;
        desc.vertex.entryPoint = Wgpu::View("main");
        desc.vertex.bufferCount = layouts.size();
        desc.vertex.buffers = layouts.empty() ? nullptr : layouts.data();
        desc.primitive.topology = ToTopology(mode);
        desc.primitive.frontFace = ToFrontFace(gl->GetFrontFaceMode());
        desc.primitive.cullMode =
            gl->IsCapabilityEnabled(CapabilityInput::CullFace) ? ToCullMode(gl->GetCullFaceMode())
                                                               : WGPUCullMode_None;
        desc.depthStencil = hasDepth ? &depthState : nullptr;
        desc.multisample.count = 1;
        desc.multisample.mask = 0xFFFFFFFFu;
        desc.fragment = &fragment;

        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(m_device, &desc);
        if (!pipeline) {
            return nullptr;
        }
        WgpuPipeline entry;
        entry.pipeline = pipeline;
        // The auto-generated group-0 layout is shared by all state variants of this
        // program (same shaders); keep it for building per-draw bind groups.
        if (prog->HasResources()) {
            entry.group0Layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
        }
        auto [ins, ok] = m_pipelineCache.emplace(key, entry);
        return &ins->second;
    }

    // Shared: (re)upload a GL buffer to a cached WGPU buffer. Re-uploads when the GL
    // buffer's change serial advances (glBufferData/SubData/map-flush); recreates the
    // WGPU buffer when the size changes. Returns the GPU buffer (or null).
    WGPUBuffer WebGPURenderer::GetOrCreateBuffer(
        UnorderedMap<const MG_State::GLState::BufferObject*, WgpuBuffer>& cache,
        MG_State::GLState::BufferObject& buffer, WGPUBufferUsage usage) {
        const Uint64 serial = buffer.GetChangeSerial();
        const SizeT size = buffer.GetSize();
        const auto& data = buffer.GetDataReadOnly();
        const Uint64 allocSize = (static_cast<Uint64>(size) + 3u) & ~Uint64(3); // 4-byte aligned
        auto it = cache.find(&buffer);
        if (it != cache.end()) {
            WgpuBuffer& cb = it->second;
            if (cb.serial == serial) {
                return cb.buffer; // up to date
            }
            if (cb.buffer && cb.size == allocSize && data) {
                // Same size, new contents -> re-upload in place.
                wgpuQueueWriteBuffer(m_queue, cb.buffer, 0, data->data(), size & ~SizeT(3));
                cb.serial = serial;
                return cb.buffer;
            }
            if (cb.buffer) wgpuBufferRelease(cb.buffer);
            cache.erase(it);
        }
        if (size == 0 || !data) {
            return nullptr;
        }
        WGPUBufferDescriptor bd{};
        bd.usage = usage | WGPUBufferUsage_CopyDst;
        bd.size = allocSize;
        WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!buf) {
            return nullptr;
        }
        wgpuQueueWriteBuffer(m_queue, buf, 0, data->data(), size & ~SizeT(3));
        cache.emplace(&buffer, WgpuBuffer{buf, serial, allocSize});
        return buf;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer) {
        return GetOrCreateBuffer(m_vertexBufferCache, buffer, WGPUBufferUsage_Vertex);
    }

    WGPUBuffer WebGPURenderer::GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer) {
        return GetOrCreateBuffer(m_indexBufferCache, buffer, WGPUBufferUsage_Index);
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
        // RenderAttachment so a texture can also be an FBO color attachment;
        // CopySrc so glGetTexImage/glGetTextureImage can copy it out.
        td.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                   WGPUTextureUsage_RenderAttachment;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {w, h, 1};
        td.format = fmt;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        WGPUTexture tex = wgpuDeviceCreateTexture(m_device, &td);
        if (!tex) {
            return nullptr;
        }
        // Upload level-0 CPU data if present; FBO attachments allocated without data
        // (glTexImage2D NULL) just stay zero-initialized and get rendered into.
        UploadTextureLevel0(tex, *mip, w, h);
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
        // Resolve the draw target first: the pipeline (color format + has-depth) and the
        // pass attachments both depend on it.
        if (!ResolveDrawTarget()) {
            return nullptr;
        }
        const WgpuPipeline* p = GetOrCreatePipeline(program, vao, mode);
        if (!p) {
            return nullptr;
        }
        // Draw into a Load render pass so a prior glClear is preserved. The depth
        // attachment is present only if the current target has one (matching the
        // pipeline's depthStencil state).
        WGPURenderPassColorAttachment color{};
        color.view = m_curColorView;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDepthStencilAttachment depth{};
        const Bool hasDepth = (m_curDepthView != nullptr);
        if (hasDepth) {
            depth.view = m_curDepthView;
            depth.depthLoadOp = WGPULoadOp_Load;
            depth.depthStoreOp = WGPUStoreOp_Store;
        }
        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;
        rp.depthStencilAttachment = hasDepth ? &depth : nullptr;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, p->pipeline);

        // Viewport (GL bottom-left origin -> WebGPU top-left, against the target height).
        // Only when the app set a non-empty viewport; otherwise WebGPU's default covers
        // the whole target.
        auto* gl = MG_State::pGLContext.get();
        const auto& vp = gl->GetViewport();
        if (vp.z() > 0 && vp.w() > 0) {
            const float vy = static_cast<float>(m_curHeight) - static_cast<float>(vp.y()) -
                             static_cast<float>(vp.w());
            wgpuRenderPassEncoderSetViewport(pass, static_cast<float>(vp.x()), vy,
                                             static_cast<float>(vp.z()), static_cast<float>(vp.w()), 0.0f,
                                             1.0f);
        }
        // Scissor (same Y flip) when the scissor test is enabled.
        if (gl->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            const auto& sc = gl->GetScissorBox();
            if (sc.z() > 0 && sc.w() > 0) {
                Int32 sy = static_cast<Int32>(m_curHeight) - sc.y() - sc.w();
                if (sy < 0) sy = 0;
                wgpuRenderPassEncoderSetScissorRect(pass, static_cast<Uint32>(sc.x() < 0 ? 0 : sc.x()),
                                                    static_cast<Uint32>(sy), static_cast<Uint32>(sc.z()),
                                                    static_cast<Uint32>(sc.w()));
            }
        }

        // Build the group-0 bind group from current state: global UBO + sampled
        // textures. Rebuilt per draw (textures/UBO contents are runtime state).
        const WgpuProgram* prog = GetOrCreateProgram(program);
        if (prog && prog->HasResources() && p->group0Layout) {
            std::vector<WGPUBindGroupEntry> entries;
            if (prog->globalUboBinding >= 0 && prog->globalUboSize > 0) {
                // Fresh per-draw UBO buffer with this draw's uniforms (see m_frameUboBuffers).
                const Uint64 uboSize = (static_cast<Uint64>(prog->globalUboSize) + 15u) & ~Uint64(15);
                WGPUBufferDescriptor bd{};
                bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                bd.size = uboSize;
                WGPUBuffer ubo = wgpuDeviceCreateBuffer(m_device, &bd);
                m_frameUboBuffers.push_back(ubo);
                const void* uboData = program.GetUBOData();
                const Uint sz = program.GetUBOSize();
                if (ubo && uboData && sz > 0) {
                    wgpuQueueWriteBuffer(m_queue, ubo, 0, uboData, sz & ~Uint(3));
                }
                WGPUBindGroupEntry e{};
                e.binding = static_cast<Uint32>(prog->globalUboBinding);
                e.buffer = ubo;
                e.size = uboSize;
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
            if (ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger) == WGPUVertexFormat_Force32 || !a.Buffer) continue;
            WGPUBuffer vb = GetOrCreateVertexBuffer(*a.Buffer);
            if (!vb) continue;
            wgpuRenderPassEncoderSetVertexBuffer(pass, slot, vb, static_cast<uint64_t>(a.Offset),
                                                 WGPU_WHOLE_SIZE);
            ++slot;
        }
        return pass;
    }

    void WebGPURenderer::DrawArrays(GLenum mode, GLint first, GLsizei count) {
        DrawArraysInstanced(mode, first, count, 1, 0);
    }

    void WebGPURenderer::DrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                                             GLsizei instanceCount, GLuint baseInstance) {
        if (!m_device || !MG_State::pGLContext || count <= 0 || instanceCount <= 0) {
            return;
        }
        if (baseInstance != 0) {
            MGLOG_W("DirectWebGPU: non-zero baseInstance (%u) unsupported (needs indirect-first-instance); "
                    "using 0", baseInstance);
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
        wgpuRenderPassEncoderDraw(pass, static_cast<Uint32>(count), static_cast<Uint32>(instanceCount),
                                  static_cast<Uint32>(first), 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    void WebGPURenderer::DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        DrawElementsInstanced(mode, count, type, indices, 1, 0, 0);
    }

    void WebGPURenderer::DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                               GLsizei instanceCount, GLint baseVertex, GLuint baseInstance) {
        if (!m_device || !MG_State::pGLContext || count <= 0 || instanceCount <= 0) {
            return;
        }
        const WGPUIndexFormat indexFormat = ToIndexFormat(type);
        if (indexFormat == WGPUIndexFormat_Undefined) {
            MGLOG_E("DirectWebGPU: unsupported index type 0x%x (GL_UNSIGNED_BYTE not in WebGPU)", type);
            return;
        }
        if (baseInstance != 0) {
            MGLOG_W("DirectWebGPU: non-zero baseInstance (%u) unsupported (needs indirect-first-instance); "
                    "using 0", baseInstance);
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
        // `indices` is a byte offset into the bound element array buffer; baseVertex is
        // added to every index by drawIndexed.
        const uint64_t byteOffset = reinterpret_cast<uintptr_t>(indices);
        wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer, indexFormat, byteOffset, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(pass, static_cast<Uint32>(count),
                                         static_cast<Uint32>(instanceCount), 0,
                                         static_cast<int32_t>(baseVertex), 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    void WebGPURenderer::MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                                     const void* const* indices, GLsizei drawcount,
                                                     const GLint* basevertex) {
        if (!m_device || !MG_State::pGLContext || drawcount <= 0 || !count || !indices) {
            return;
        }
        const WGPUIndexFormat indexFormat = ToIndexFormat(type);
        if (indexFormat == WGPUIndexFormat_Undefined) {
            MGLOG_E("DirectWebGPU: MultiDrawElementsBaseVertex unsupported index type 0x%x", type);
            return;
        }
        const Uint32 indexSize = (type == GL_UNSIGNED_INT) ? 4u : 2u;
        const auto& programPtr = MG_State::pGLContext->GetCurrentProgram();
        const auto& vaoPtr = MG_State::pGLContext->GetBoundVertexArray();
        if (!programPtr || !vaoPtr) {
            return;
        }
        const auto& iboPtr = vaoPtr->GetIndexBufferBindingSlot().GetBoundObject();
        if (!iboPtr) {
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
        wgpuRenderPassEncoderSetIndexBuffer(pass, indexBuffer, indexFormat, 0, WGPU_WHOLE_SIZE);
        // One drawIndexed per sub-draw: firstIndex from the per-draw byte offset, plus
        // its baseVertex. (Emulates the multi-draw on WebGPU's single-draw API.)
        for (GLsizei i = 0; i < drawcount; ++i) {
            if (count[i] <= 0) continue;
            const uint64_t byteOffset = reinterpret_cast<uintptr_t>(indices[i]);
            const Uint32 firstIndex = static_cast<Uint32>(byteOffset / indexSize);
            const int32_t bv = basevertex ? static_cast<int32_t>(basevertex[i]) : 0;
            wgpuRenderPassEncoderDrawIndexed(pass, static_cast<Uint32>(count[i]), 1, firstIndex, bv, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    namespace {
        struct ReadbackState {
            WGPUMapAsyncStatus status = static_cast<WGPUMapAsyncStatus>(0);
        };
        void OnBufferMapped(WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
            if (ud1) static_cast<ReadbackState*>(ud1)->status = status;
            mobilegl_jspi_signal();
        }
        struct WorkDoneState {
            WGPUQueueWorkDoneStatus status = static_cast<WGPUQueueWorkDoneStatus>(0);
        };
        void OnWorkDone(WGPUQueueWorkDoneStatus status, void* ud1, void*) {
            if (ud1) static_cast<WorkDoneState*>(ud1)->status = status;
            mobilegl_jspi_signal();
        }
        constexpr Uint32 AlignUp256(Uint32 v) { return (v + 255u) & ~255u; }
    } // namespace

    void WebGPURenderer::Finish() {
        if (!m_device) {
            return;
        }
        // Make sure any in-progress frame's commands are submitted, then wait.
        if (m_frameActive) {
            FlushFrame();
        }
        WorkDoneState st;
        WGPUQueueWorkDoneCallbackInfo ci{};
        ci.mode = WGPUCallbackMode_AllowSpontaneous;
        ci.callback = &OnWorkDone;
        ci.userdata1 = &st;
        wgpuQueueOnSubmittedWorkDone(m_queue, ci);
        mobilegl_jspi_wait(); // suspends until OnWorkDone signals
    }

    Bool WebGPURenderer::ReadTextureToCPU(WGPUTexture tex, Uint32 srcX, Uint32 srcY, Uint32 w, Uint32 h,
                                          Bool srcIsBgra, GLenum dstFormat, Bool flipY, void* out) {
        if (m_readbackInFlight) {
            MGLOG_E("DirectWebGPU: reentrant readback while one is in flight; ignoring");
            return false;
        }
        const Uint32 bytesPerRow = AlignUp256(w * 4u); // WebGPU requires 256B row alignment
        const Uint64 bufSize = static_cast<Uint64>(bytesPerRow) * h;

        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bd.size = bufSize;
        WGPUBuffer readback = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!readback) {
            return false;
        }

        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
        WGPUTexelCopyTextureInfo src{};
        src.texture = tex;
        src.origin = {srcX, srcY, 0};
        WGPUTexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bytesPerRow;
        dst.layout.rowsPerImage = h;
        WGPUExtent3D ext{w, h, 1};
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(m_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);

        // Map asynchronously and block via JSPI until the callback signals.
        ReadbackState st;
        WGPUBufferMapCallbackInfo ci{};
        ci.mode = WGPUCallbackMode_AllowSpontaneous;
        ci.callback = &OnBufferMapped;
        ci.userdata1 = &st;
        m_readbackInFlight = true;
        wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, bufSize, ci);
        mobilegl_jspi_wait();
        m_readbackInFlight = false;

        Bool ok = false;
        if (st.status == WGPUMapAsyncStatus_Success) {
            const auto* mapped = static_cast<const Uint8*>(
                wgpuBufferGetConstMappedRange(readback, 0, bufSize));
            if (mapped) {
                auto* dstBytes = static_cast<Uint8*>(out);
                const Bool wantBgra = (dstFormat == GL_BGRA);
                const Bool swapRB = (srcIsBgra != wantBgra);
                for (Uint32 row = 0; row < h; ++row) {
                    const Uint64 srcRowIdx = flipY ? (h - 1 - row) : row;
                    const Uint8* srcRow = mapped + srcRowIdx * bytesPerRow;
                    Uint8* dstRow = dstBytes + static_cast<Uint64>(row) * w * 4u;
                    if (swapRB) {
                        for (Uint32 px = 0; px < w; ++px) {
                            dstRow[px * 4 + 0] = srcRow[px * 4 + 2];
                            dstRow[px * 4 + 1] = srcRow[px * 4 + 1];
                            dstRow[px * 4 + 2] = srcRow[px * 4 + 0];
                            dstRow[px * 4 + 3] = srcRow[px * 4 + 3];
                        }
                    } else {
                        std::memcpy(dstRow, srcRow, w * 4u);
                    }
                }
                ok = true;
            }
            wgpuBufferUnmap(readback);
        } else {
            MGLOG_E("DirectWebGPU: buffer map failed (status=%d)", static_cast<int>(st.status));
        }
        wgpuBufferRelease(readback);
        return ok;
    }

    void WebGPURenderer::ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                    GLenum type, void* pixels) {
        if (!m_device || !pixels || width <= 0 || height <= 0) {
            return;
        }
        if (type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_BGRA)) {
            MGLOG_E("DirectWebGPU: ReadPixels only supports GL_RGBA/GL_BGRA + GL_UNSIGNED_BYTE (got "
                    "format=0x%x type=0x%x)", format, type);
            return;
        }
        EnsureOffscreenTarget();
        if (!m_offscreenTexture) {
            return;
        }
        // Flush pending draws so the offscreen target reflects them before the copy.
        if (m_frameActive) {
            FlushFrame();
        }
        const Uint32 w = static_cast<Uint32>(width);
        const Uint32 h = static_cast<Uint32>(height);
        // GL's origin is bottom-left; WebGPU textures are top-left. The GL row range
        // [y, y+h) maps to texel rows [m_height-y-h, m_height-y); rows are flipped on
        // copy-out (flipY=true).
        Int32 texY = static_cast<Int32>(m_height) - y - static_cast<Int32>(h);
        if (texY < 0) texY = 0;
        const Bool surfaceIsBgra = (m_format == WGPUTextureFormat_BGRA8Unorm ||
                                    m_format == WGPUTextureFormat_BGRA8UnormSrgb);
        ReadTextureToCPU(m_offscreenTexture, static_cast<Uint32>(x), static_cast<Uint32>(texY), w, h,
                         surfaceIsBgra, format, /*flipY=*/true, pixels);
    }

    void WebGPURenderer::GetTextureImage(MG_State::GLState::ITextureObject& texture,
                                         TextureUploadTarget uploadTarget, GLint level, GLenum format,
                                         GLenum type, GLsizei bufSize, void* pixels) {
        if (!m_device || !pixels) {
            return;
        }
        if (type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_BGRA)) {
            MGLOG_E("DirectWebGPU: GetTextureImage only supports GL_RGBA/GL_BGRA + GL_UNSIGNED_BYTE");
            return;
        }
        if (level != 0 || uploadTarget != TextureUploadTarget::Texture2D) {
            MGLOG_E("DirectWebGPU: GetTextureImage only supports 2D level 0 for now");
            return;
        }
        const WgpuTexture* wt = GetOrCreateTexture(texture);
        if (!wt || !wt->texture) {
            return;
        }
        const Uint32 w = wt->width, h = wt->height;
        if (static_cast<Uint64>(w) * h * 4u > static_cast<Uint64>(bufSize > 0 ? bufSize : INT32_MAX)) {
            MGLOG_E("DirectWebGPU: GetTextureImage bufSize too small (%d)", bufSize);
            return;
        }
        // Texture data is returned in texture order (top-to-bottom): no flip. Our
        // texture formats (RGBA8/SRGB8Alpha8) are RGBA-order, so srcIsBgra=false.
        ReadTextureToCPU(wt->texture, 0, 0, w, h, /*srcIsBgra=*/false, format, /*flipY=*/false, pixels);
    }

    void WebGPURenderer::GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
        if (!MG_State::pGLContext) {
            return;
        }
        if (target != GL_TEXTURE_2D) {
            MGLOG_E("DirectWebGPU: GetTexImage only supports GL_TEXTURE_2D for now (got 0x%x)", target);
            return;
        }
        auto bound = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                         .GetBindingSlot(TextureTarget::Texture2D)
                         .GetBoundObject();
        if (!bound) {
            MGLOG_E("DirectWebGPU: GetTexImage with no texture bound to GL_TEXTURE_2D");
            return;
        }
        GetTextureImage(*bound, TextureUploadTarget::Texture2D, level, format, type, INT32_MAX, pixels);
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
