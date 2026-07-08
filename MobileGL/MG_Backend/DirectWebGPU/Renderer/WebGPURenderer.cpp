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
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <unordered_set>

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

        // GL vertex attribute -> WGPU vertex format. WebGPU's 8/16-bit formats only
        // come in x2/x4; a 3-component attribute of those types is promoted to the x4
        // format (it reads one extra padding byte/short — GL interleaved layouts pad
        // such attributes to 4 bytes, e.g. Minecraft's 3xGL_BYTE normal at offset 32
        // of a 36-byte stride — and the shader's vec3 input ignores the 4th
        // component). Silently dropping them instead zeroes e.g. every entity normal
        // (directional entity lighting collapses to its 0.4 ambient floor). Size 1
        // stays unsupported (returns the Force32 sentinel; the attribute is skipped).
        // `normalized` picks Unorm/Snorm; `isInteger` (glVertexAttribIPointer) picks
        // Uint/Sint; otherwise float.
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
                if (size == 3 || size == 4) return WGPUVertexFormat_Float16x4;
                break;
            case DataType::Uint8:
                if (size == 2) return n ? WGPUVertexFormat_Unorm8x2 : WGPUVertexFormat_Uint8x2;
                if (size == 3 || size == 4) return n ? WGPUVertexFormat_Unorm8x4 : WGPUVertexFormat_Uint8x4;
                break;
            case DataType::Int8:
                if (size == 2) return n ? WGPUVertexFormat_Snorm8x2 : WGPUVertexFormat_Sint8x2;
                if (size == 3 || size == 4) return n ? WGPUVertexFormat_Snorm8x4 : WGPUVertexFormat_Sint8x4;
                break;
            case DataType::Uint16:
                if (size == 2) return n ? WGPUVertexFormat_Unorm16x2 : WGPUVertexFormat_Uint16x2;
                if (size == 3 || size == 4) return n ? WGPUVertexFormat_Unorm16x4 : WGPUVertexFormat_Uint16x4;
                break;
            case DataType::Int16:
                if (size == 2) return n ? WGPUVertexFormat_Snorm16x2 : WGPUVertexFormat_Sint16x2;
                if (size == 3 || size == 4) return n ? WGPUVertexFormat_Snorm16x4 : WGPUVertexFormat_Sint16x4;
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

        // Base type a WGPU vertex format presents to the shader: 0 = float (incl. unorm/
        // snorm/float16, all read as f32), 1 = i32, 2 = u32. Used to detect a mismatch
        // with what the shader declares at a @location (WebGPU rejects the pipeline then).
        int VertexFormatBaseType(WGPUVertexFormat f) {
            switch (f) {
            case WGPUVertexFormat_Uint8x2: case WGPUVertexFormat_Uint8x4:
            case WGPUVertexFormat_Uint16x2: case WGPUVertexFormat_Uint16x4:
            case WGPUVertexFormat_Uint32: case WGPUVertexFormat_Uint32x2:
            case WGPUVertexFormat_Uint32x3: case WGPUVertexFormat_Uint32x4:
                return 2;
            case WGPUVertexFormat_Sint8x2: case WGPUVertexFormat_Sint8x4:
            case WGPUVertexFormat_Sint16x2: case WGPUVertexFormat_Sint16x4:
            case WGPUVertexFormat_Sint32: case WGPUVertexFormat_Sint32x2:
            case WGPUVertexFormat_Sint32x3: case WGPUVertexFormat_Sint32x4:
                return 1;
            default:
                return 0;
            }
        }

        // Float32x{size} vertex format used when widening an integer attribute to float.
        WGPUVertexFormat FloatFormatForSize(int size) {
            switch (size) {
            case 1: return WGPUVertexFormat_Float32;
            case 2: return WGPUVertexFormat_Float32x2;
            case 3: return WGPUVertexFormat_Float32x3;
            case 4: return WGPUVertexFormat_Float32x4;
            default: return WGPUVertexFormat_Force32;
            }
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

        // Resolved WGPU color format + its byte size per texel (row-pitch input). WebGPU
        // has no 3-channel rgb8/rgb16f, so RGB internal formats stay unsupported (would
        // need caller-side expansion to RGBA). Depth formats are resolved separately
        // (GetOrCreateFboDepth); this table is color/sampled targets only.
        struct ColorFormatInfo {
            WGPUTextureFormat format = WGPUTextureFormat_Undefined;
            Uint32 bytesPerTexel = 0;    // WGPU texel size (write side)
            Uint32 srcBytesPerTexel = 0; // GL source texel size; 0 => same as bytesPerTexel
        };
        ColorFormatInfo ToColorFormat(TextureInternalFormat fmt) {
            using F = TextureInternalFormat;
            switch (fmt) {
            case F::R8: case F::Red:            return {WGPUTextureFormat_R8Unorm, 1};
            case F::RG8: case F::RG:            return {WGPUTextureFormat_RG8Unorm, 2};
            case F::RGBA8: case F::RGBA:        return {WGPUTextureFormat_RGBA8Unorm, 4};
            case F::SRGB8Alpha8:                return {WGPUTextureFormat_RGBA8UnormSrgb, 4};
            // WebGPU has no 3-channel rgb8: store as RGBA8 and expand the source pixels
            // (alpha=1) on upload. sRGB8 likewise -> RGBA8UnormSrgb.
            case F::RGB8: case F::RGB:          return {WGPUTextureFormat_RGBA8Unorm, 4, 3};
            case F::SRGB8:                      return {WGPUTextureFormat_RGBA8UnormSrgb, 4, 3};
            case F::RGB10A2:                    return {WGPUTextureFormat_RGB10A2Unorm, 4};
            // WebGPU has no 16-bit unorm formats; map to the float variants. Iris writes
            // normalized [0,1] values into these render targets, so float storage round-
            // trips them fine (with float precision instead of 16-bit unorm quantization).
            case F::R16:                        return {WGPUTextureFormat_R16Float, 2};
            case F::RG16:                       return {WGPUTextureFormat_RG16Float, 4};
            case F::RGBA16:                     return {WGPUTextureFormat_RGBA16Float, 8};
            case F::R16F:                       return {WGPUTextureFormat_R16Float, 2};
            case F::RG16F:                      return {WGPUTextureFormat_RG16Float, 4};
            case F::RGBA16F:                    return {WGPUTextureFormat_RGBA16Float, 8};
            case F::R32F:                       return {WGPUTextureFormat_R32Float, 4};
            case F::RG32F:                      return {WGPUTextureFormat_RG32Float, 8};
            case F::RGBA32F:                    return {WGPUTextureFormat_RGBA32Float, 16};
            case F::R11FG11FB10F:               return {WGPUTextureFormat_RG11B10Ufloat, 4};
            default:                            return {WGPUTextureFormat_Undefined, 0};
            }
        }

        // WebGPU forbids setting a blend state on a non-blendable color target. 8-bit
        // unorm, rgb10a2unorm, 16-bit float and rg11b10ufloat are blendable in core;
        // 32-bit float needs the float32-blendable feature (absent in this toolchain),
        // so treat it as non-blendable and just drop blending for those targets.
        Bool IsBlendableFormat(WGPUTextureFormat f) {
            switch (f) {
            case WGPUTextureFormat_R8Unorm:
            case WGPUTextureFormat_RG8Unorm:
            case WGPUTextureFormat_RGBA8Unorm:
            case WGPUTextureFormat_RGBA8UnormSrgb:
            case WGPUTextureFormat_BGRA8Unorm:
            case WGPUTextureFormat_BGRA8UnormSrgb:
            case WGPUTextureFormat_RGB10A2Unorm:
            case WGPUTextureFormat_R16Float:
            case WGPUTextureFormat_RG16Float:
            case WGPUTextureFormat_RGBA16Float:
            case WGPUTextureFormat_RG11B10Ufloat:
                return true;
            default:
                return false; // 32-bit float and integer targets
            }
        }

        // GL depth internal format -> WGPU depth format (Undefined if not a depth format).
        // Depth textures are created sampleable so Iris can read depthtex*/shadowtex*.
        WGPUTextureFormat ToDepthFormat(TextureInternalFormat fmt) {
            using F = TextureInternalFormat;
            switch (fmt) {
            case F::DepthComponent16:  return WGPUTextureFormat_Depth16Unorm;
            case F::DepthComponent24:  return WGPUTextureFormat_Depth24Plus;
            case F::DepthComponent32:  // not a core GL format; treat as 32F
            case F::DepthComponent32F: return WGPUTextureFormat_Depth32Float;
            case F::DepthComponent:    return WGPUTextureFormat_Depth32Float; // generic depth
            case F::Depth24Stencil8:   return WGPUTextureFormat_Depth24PlusStencil8;
            default:                   return WGPUTextureFormat_Undefined;
            }
        }

        WGPUTextureFormat ToSrgbViewFormat(WGPUTextureFormat fmt) {
            switch (fmt) {
            case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
            case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
            default: return fmt;
            }
        }

        WGPUTextureFormat ToLinearViewFormat(WGPUTextureFormat fmt) {
            switch (fmt) {
            case WGPUTextureFormat_RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8Unorm;
            case WGPUTextureFormat_BGRA8UnormSrgb: return WGPUTextureFormat_BGRA8Unorm;
            default: return fmt;
            }
        }

        Uint32 CountValidMipLevels(const MG_State::GLState::TextureObjectMipmap& mip, TextureUploadTarget target) {
            const Uint total = mip.GetMipmapLevelCount();
            Uint32 valid = 0;
            for (Uint level = 0; level < total; ++level) {
                const IntVec3 dim = mip.GetMipmapTexelSize(target, level);
                if (dim.x() <= 0 || dim.y() <= 0) {
                    break;
                }
                ++valid;
            }
            return valid;
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

        // RemapClipSpace negates gl_Position.y on every draw (so render targets stay
        // in GL texel order — see its comment). The flip mirrors screen-space triangle
        // orientation, so GL's front-face winding must be inverted here.
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
    namespace {
        std::unordered_set<std::string> g_loggedMissingSamplerUnits;
    }

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
        for (auto& [k, b] : m_repackedVertexBufferCache) { if (b.buffer) wgpuBufferRelease(b.buffer); }
        m_repackedVertexBufferCache.clear();
        for (auto& [k, b] : m_indexBufferCache) { if (b.buffer) wgpuBufferRelease(b.buffer); }
        m_indexBufferCache.clear();
        for (auto& [k, p] : m_pipelineCache) {
            if (p.group0Layout) wgpuBindGroupLayoutRelease(p.group0Layout);
            if (p.pipelineLayout) wgpuPipelineLayoutRelease(p.pipelineLayout);
            if (p.pipeline) wgpuRenderPipelineRelease(p.pipeline);
        }
        m_pipelineCache.clear();
        for (auto& [k, t] : m_textureCache) {
            if (t.attachmentSrgbView) wgpuTextureViewRelease(t.attachmentSrgbView);
            if (t.attachmentView) wgpuTextureViewRelease(t.attachmentView);
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
        if (m_offscreenSrgbView) { wgpuTextureViewRelease(m_offscreenSrgbView); m_offscreenSrgbView = nullptr; }
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
        if (m_offscreenSrgbView) { wgpuTextureViewRelease(m_offscreenSrgbView); m_offscreenSrgbView = nullptr; }
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
        const WGPUTextureFormat srgbViewFormat = ToSrgbViewFormat(m_format);
        if (srgbViewFormat != m_format) {
            td.viewFormatCount = 1;
            td.viewFormats = &srgbViewFormat;
        }
        m_offscreenTexture = wgpuDeviceCreateTexture(m_device, &td);
        m_offscreenView = m_offscreenTexture ? wgpuTextureCreateView(m_offscreenTexture, nullptr) : nullptr;
        if (m_offscreenTexture && srgbViewFormat != m_format) {
            WGPUTextureViewDescriptor vd{};
            vd.format = srgbViewFormat;
            vd.dimension = WGPUTextureViewDimension_2D;
            vd.baseMipLevel = 0;
            vd.mipLevelCount = 1;
            vd.baseArrayLayer = 0;
            vd.arrayLayerCount = 1;
            vd.aspect = WGPUTextureAspect_All;
            m_offscreenSrgbView = wgpuTextureCreateView(m_offscreenTexture, &vd);
        }

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
        for (auto& c : m_curColor) {
            c.view = nullptr;
            c.format = WGPUTextureFormat_Undefined;
        }
        m_curColorCount = 0;
        const auto& fbo = gl->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (!fbo || fbo->IsDefaultFramebuffer()) {
            EnsureOffscreenTarget();
            const Bool framebufferSrgb = gl->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb);
            WGPUTextureView colorView = m_offscreenView;
            WGPUTextureFormat colorFormat = m_format;
            if (framebufferSrgb) {
                const WGPUTextureFormat srgbFormat = ToSrgbViewFormat(m_format);
                if (srgbFormat != m_format && m_offscreenSrgbView) {
                    colorView = m_offscreenSrgbView;
                    colorFormat = srgbFormat;
                }
            }
            if (!colorView) {
                return false;
            }
            m_curColor[0] = {colorView, colorFormat};
            m_curColorCount = 1;
            m_curDepthView = m_depthView;
            m_curDepthFormat = kDepthFormat;
            m_curWidth = m_width;
            m_curHeight = m_height;
            m_curIsDefault = true;
            return true;
        }
        // User FBO: resolve each active draw buffer (glDrawBuffers) to its texture color
        // attachment. Slot index == fragment output @location; a None / unusable draw
        // buffer stays a hole (null view). Renderbuffer color attachments are still
        // unsupported.
        const auto& drawBuffers = fbo->GetDrawBuffers();
        const Bool framebufferSrgb = gl->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb);
        Uint32 w = 0, h = 0;
        Bool any = false;
        Uint slotCount = 0;
        for (Uint i = 0; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS && i < kMaxColorTargets;
             ++i) {
            const FramebufferAttachmentType att = drawBuffers[i];
            if (att == FramebufferAttachmentType::None) {
                continue; // hole
            }
            const auto& color = fbo->GetAttachment(att);
            if (!color.IsValid() || color.IsEmpty() || !color.IsTexture()) {
                MGLOG_E("DirectWebGPU: FBO %u draw buffer %u has no texture attachment "
                        "(renderbuffer color unsupported)", fbo->GetExternalIndex(), i);
                continue; // leave a hole; the shader output there is dropped
            }
            const WgpuTexture* wt = GetOrCreateTexture(*color.GetTexture());
            if (!wt || !wt->attachmentView) {
                continue;
            }
            // GL sRGB-encodes fragment outputs only into sRGB-format attachments while
            // GL_FRAMEBUFFER_SRGB is enabled; a linear (UNORM) attachment is never
            // encoded (raw bytes). attachmentSrgbView is non-null only for sRGB textures.
            if (wt->attachmentSrgbView && framebufferSrgb) {
                m_curColor[i] = {wt->attachmentSrgbView, ToSrgbViewFormat(wt->format)};
            } else {
                m_curColor[i] = {wt->attachmentView, ToLinearViewFormat(wt->format)};
            }
            slotCount = i + 1;
            if (!any) {
                w = wt->width;
                h = wt->height;
                any = true;
            }
        }
        if (!any) {
            MGLOG_E("DirectWebGPU: FBO %u has no usable texture color attachment",
                    fbo->GetExternalIndex());
            return false;
        }
        m_curColorCount = slotCount;
        m_curWidth = w;
        m_curHeight = h;
        m_curIsDefault = false;
        // Depth/stencil attachment. A texture attachment (depthtex*) becomes a real
        // sampleable depth texture so later passes can read it; a renderbuffer (or the
        // resolve failing) falls back to a transient depth buffer.
        const auto& depthAtt = fbo->GetAttachment(FramebufferAttachmentType::Depth);
        if (depthAtt.IsValid() && !depthAtt.IsEmpty()) {
            const WgpuTexture* dt =
                (depthAtt.IsTexture() && depthAtt.GetTexture()) ? GetOrCreateTexture(*depthAtt.GetTexture())
                                                                : nullptr;
            if (dt && dt->isDepth && dt->attachmentView) {
                m_curDepthView = dt->attachmentView;
                m_curDepthFormat = dt->format;
            } else {
                m_curDepthView = GetOrCreateFboDepth(*fbo, w, h);
                m_curDepthFormat = kDepthFormat;
            }
        } else {
            m_curDepthView = nullptr;
            m_curDepthFormat = kDepthFormat;
        }
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
        // Render targets are stored in GL texel order (see RemapClipSpace), so GL's
        // bottom-left rectangles map to texel rows directly.
        (void)srcH; (void)dstH;
        const Int32 sx = std::min(srcX0, srcX1), sy = std::min(srcY0, srcY1);
        const Int32 dx = std::min(dstX0, dstX1), dy = std::min(dstY0, dstY1);
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
        // Everything recorded so far is submitted; resources marked with the old
        // serial can now be overwritten safely (queue writes land after the submit).
        ++m_flushSerial;
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

        // glClear(GL_COLOR_BUFFER_BIT) clears every active draw buffer, so build one
        // color attachment per resolved slot (holes -> null attachment).
        std::vector<WGPURenderPassColorAttachment> colors;
        if (clearColor) {
            const auto& c = gl->GetClearColor();
            colors.resize(m_curColorCount);
            for (Uint i = 0; i < m_curColorCount; ++i) {
                WGPURenderPassColorAttachment& a = colors[i];
                a = {};
                a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
                if (m_curColor[i].view) {
                    a.view = m_curColor[i].view;
                    a.loadOp = WGPULoadOp_Clear;
                    a.storeOp = WGPUStoreOp_Store;
                    a.clearValue = {static_cast<double>(c[0]), static_cast<double>(c[1]),
                                    static_cast<double>(c[2]), static_cast<double>(c[3])};
                } else {
                    a.view = nullptr; // hole: loadOp/storeOp must stay Undefined
                }
            }
        }

        WGPURenderPassDepthStencilAttachment depth{};
        if (doDepth) {
            depth.view = m_curDepthView;
            depth.depthLoadOp = WGPULoadOp_Clear;
            depth.depthStoreOp = WGPUStoreOp_Store;
            depth.depthClearValue = gl->GetClearDepth();
        }

        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = colors.size();
        rp.colorAttachments = colors.empty() ? nullptr : colors.data();
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
        // The offscreen is stored in GL texel order (row 0 = window bottom; see
        // RemapClipSpace) while the canvas presents row 0 at the top, so copy row by
        // row, reversed. (copyTextureToTexture cannot flip; a sampled blit can replace
        // this if the per-row copies ever matter for performance.)
        if (AcquireSurfaceView()) {
            for (Uint32 row = 0; row < m_height; ++row) {
                WGPUTexelCopyTextureInfo src{};
                src.texture = m_offscreenTexture;
                src.origin = {0, row, 0};
                WGPUTexelCopyTextureInfo dst{};
                dst.texture = m_frameTexture;
                dst.origin = {0, m_height - 1 - row, 0};
                WGPUExtent3D ext{m_width, 1, 1};
                wgpuCommandEncoderCopyTextureToTexture(m_encoder, &src, &dst, &ext);
            }
        }
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(m_encoder, nullptr);
        wgpuQueueSubmit(m_queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        EndFrame();
        // On the web the configured canvas surface is presented implicitly when
        // control returns to the browser event loop.
    }

    // WebGPU merges all shader stages into a single shared @group(0), but glslang
    // numbers each stage's resources independently from 0. So a UBO at binding 2 in
    // the vertex stage and a texture at binding 2 in the fragment stage collide
    // ("Binding types differ") when WebGPU builds the implicit layout. Give each
    // stage a disjoint binding range so they never overlap. (tint splits a combined
    // sampler into texture@N + sampler@N+1, but both stay within the stage's range,
    // and glslang already spaces combined samplers by 2 within a stage.)
    static constexpr Uint32 kStageBindingStride = 64;
    static Uint32 StageBindingOffset(ShaderStage stage) {
        return stage == ShaderStage::Fragment ? kStageBindingStride : 0;
    }

    // Rewrite every "@binding(N)" in tint's WGSL output to "@binding(N+offset)".
    // tint's output is deterministic and comment-free, so a textual pass is safe.
    static std::string OffsetWgslBindings(const std::string& wgsl, Uint32 offset) {
        if (offset == 0) {
            return wgsl;
        }
        static const std::string tok = "@binding(";
        std::string out;
        out.reserve(wgsl.size() + 32);
        size_t prev = 0, pos;
        while ((pos = wgsl.find(tok, prev)) != std::string::npos) {
            const size_t numStart = pos + tok.size();
            size_t numEnd = numStart;
            Uint32 n = 0;
            while (numEnd < wgsl.size() && wgsl[numEnd] >= '0' && wgsl[numEnd] <= '9') {
                n = n * 10 + static_cast<Uint32>(wgsl[numEnd] - '0');
                ++numEnd;
            }
            out.append(wgsl, prev, numStart - prev);
            out.append(std::to_string(n + offset));
            prev = numEnd;
        }
        out.append(wgsl, prev, std::string::npos);
        return out;
    }

    // tint emits each group-0 resource as a one-line declaration, e.g.
    //   @binding(0) @group(0) var<uniform> x_41 : MGL_GLOBAL_UBO;
    //   @binding(1) @group(0) var Sampler0_image : texture_2d<f32>;
    //   @binding(2) @group(0) var Sampler0_sampler : sampler;
    // Parsing the WGSL is the only reliable way to know the module's real interface:
    // tint drops the sampler half of a combined sampler when the shader only uses
    // texelFetch (the texture then becomes UnfilterableFloat with no sampler).
    struct ParsedWgslResource {
        Uint32 binding = 0;
        enum class Kind { Ubo, Texture, Sampler } kind = Kind::Texture;
        String varName;
        String typeName;
        bool depth = false;      // texture_depth_2d (from GLSL sampler2DShadow)
        bool comparison = false; // sampler_comparison
    };
    static void ParseWgslResources(const std::string& wgsl, std::vector<ParsedWgslResource>& out) {
        static const std::string tok = "@binding(";
        size_t pos = 0;
        while ((pos = wgsl.find(tok, pos)) != std::string::npos) {
            size_t ns = pos + tok.size();
            Uint32 binding = 0;
            while (ns < wgsl.size() && wgsl[ns] >= '0' && wgsl[ns] <= '9') {
                binding = binding * 10 + static_cast<Uint32>(wgsl[ns] - '0');
                ++ns;
            }
            size_t semi = wgsl.find(';', ns);
            size_t nl = wgsl.find('\n', ns);
            size_t end = (semi == std::string::npos) ? nl : ((nl != std::string::npos && nl < semi) ? nl : semi);
            if (end == std::string::npos) break;
            const std::string decl = wgsl.substr(pos, end - pos);
            pos = end;
            size_t vp = decl.find("var");
            if (vp == std::string::npos) continue;
            const bool isUniform = decl.find("var<uniform>") != std::string::npos;
            size_t after = vp + 3;
            if (after < decl.size() && decl[after] == '<') { // skip var<...>
                after = decl.find('>', after);
                if (after == std::string::npos) continue;
                ++after;
            }
            while (after < decl.size() && (decl[after] == ' ' || decl[after] == '\t')) ++after;
            size_t nameEnd = after;
            while (nameEnd < decl.size() &&
                   (std::isalnum(static_cast<unsigned char>(decl[nameEnd])) || decl[nameEnd] == '_')) {
                ++nameEnd;
            }
            ParsedWgslResource r;
            r.binding = binding;
            r.varName = decl.substr(after, nameEnd - after);
            const size_t colon = decl.find(':', nameEnd);
            r.typeName = (colon != std::string::npos) ? decl.substr(colon + 1) : "";
            if (isUniform) {
                r.kind = ParsedWgslResource::Kind::Ubo;
            } else if (r.typeName.find("texture") != std::string::npos) {
                r.kind = ParsedWgslResource::Kind::Texture;
                r.depth = r.typeName.find("texture_depth") != std::string::npos;
            } else if (r.typeName.find("sampler") != std::string::npos) {
                r.kind = ParsedWgslResource::Kind::Sampler;
                r.comparison = r.typeName.find("sampler_comparison") != std::string::npos;
            } else {
                continue;
            }
            out.push_back(r);
        }
    }

    static bool IsWgslIdentChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // Counts identifier-token occurrences of needle in haystack. Used to tell whether
    // a resource var is actually referenced (declaration + >=1 use) or merely declared:
    // tint emits the sampler half of a combined sampler even when the shader only uses
    // texelFetch, and WebGPU's auto pipeline layout omits such statically-unused
    // bindings. Binding them anyway makes the bind group (and the whole command buffer)
    // invalid, so we must skip them. This must be token-aware: tint names like x_2 are
    // common prefixes of x_20/x_21, and a substring count will falsely mark unused
    // declarations as active.
    static int CountIdentifierOccurrences(const std::string& haystack, const std::string& needle) {
        if (needle.empty()) return 0;
        int n = 0;
        for (size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + needle.size())) {
            const bool beforeOk = (p == 0) || !IsWgslIdentChar(haystack[p - 1]);
            const size_t after = p + needle.size();
            const bool afterOk = (after >= haystack.size()) || !IsWgslIdentChar(haystack[after]);
            if (beforeOk && afterOk) {
                ++n;
            }
        }
        return n;
    }

    // Two GL->WebGPU clip-space remaps, injected into tint's vertex entry point
    // (tint emits an entry that calls the original main, renamed main_1, then returns
    // a main_out holding the gl_Position var; inject right after that call):
    //  - Z: GL clips z in [-w, w]; WebGPU (like Vulkan/D3D) clips to [0, w]. Without
    //    the remap the near half of the depth range is clipped away.
    //  - Y: negate. GL's NDC +y with bottom-row-0 windows stores NDC -1 at texel
    //    row 0; WebGPU's NDC +y with top-row-0 targets would store it at row h-1,
    //    i.e. every render target would be vertically mirrored versus GL. That
    //    breaks any shader that SAMPLES a rendered target (e.g. Minecraft's
    //    GPU-rendered lightmap). Flipping y on every draw keeps all render targets
    //    in GL texel order; Present un-flips for the top-row-0 canvas.
    // (Done on the WGSL rather than in glslang/SPIR-V because that path is shared
    // with DirectVulkan, whose y-down NDC already matches GL texel order for FBOs.)
    static std::string RemapClipSpace(const std::string& wgsl, ShaderStage stage) {
        if (stage != ShaderStage::Vertex) {
            return wgsl;
        }
        static const std::string needle = "main_1();";
        const size_t p = wgsl.rfind(needle);
        if (p == std::string::npos || wgsl.find("gl_Position") == std::string::npos) {
            return wgsl;
        }
        std::string out = wgsl;
        out.insert(p + needle.size(),
                   "\n  gl_Position.y = -gl_Position.y;"
                   "\n  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;");
        return out;
    }

    // Recover the GL uniform name from tint's split var name: "Foo_image" /
    // "Foo_sampler" -> "Foo".
    static String BaseUniformName(const String& varName) {
        static const String si = "_image";
        static const String ss = "_sampler";
        if (varName.size() > si.size() &&
            varName.compare(varName.size() - si.size(), si.size(), si) == 0) {
            return varName.substr(0, varName.size() - si.size());
        }
        if (varName.size() > ss.size() &&
            varName.compare(varName.size() - ss.size(), ss.size(), ss) == 0) {
            return varName.substr(0, varName.size() - ss.size());
        }
        return varName;
    }

    // Parse the @vertex entry's "@location(N) ... : TYPE" inputs from tint's WGSL.
    // Output: (location, baseType) pairs where baseType is 0=float, 1=i32, 2=u32.
    static void ParseWgslVertexLocations(const std::string& wgsl,
                                         std::vector<std::pair<Uint32, int>>& out) {
        const size_t vp = wgsl.find("@vertex");
        if (vp == std::string::npos) {
            return;
        }
        const size_t sigStart = wgsl.find('(', vp);
        const size_t bodyStart = wgsl.find('{', vp);
        if (sigStart == std::string::npos || bodyStart == std::string::npos || sigStart > bodyStart) {
            return;
        }
        static const std::string tok = "@location(";
        size_t pos = sigStart;
        while (true) {
            const size_t loc = wgsl.find(tok, pos);
            if (loc == std::string::npos || loc > bodyStart) {
                break;
            }
            size_t ns = loc + tok.size();
            Uint32 n = 0;
            while (ns < wgsl.size() && wgsl[ns] >= '0' && wgsl[ns] <= '9') {
                n = n * 10 + static_cast<Uint32>(wgsl[ns] - '0');
                ++ns;
            }
            const size_t colon = wgsl.find(':', ns);
            const size_t end = (colon != std::string::npos) ? wgsl.find_first_of(",)", colon)
                                                            : std::string::npos;
            const std::string type =
                (colon != std::string::npos && end != std::string::npos) ? wgsl.substr(colon + 1, end - colon - 1)
                                                                        : "";
            // u32 has 'u' but no 'i'; i32 has 'i' but no 'u'; check unsigned first.
            const int baseType = (type.find('u') != std::string::npos) ? 2
                                 : (type.find('i') != std::string::npos) ? 1
                                                                         : 0;
            out.emplace_back(n, baseType);
            pos = ns;
        }
    }

    // Parse the fragment stage's output @location(N) values from tint's WGSL. tint emits
    // the entry as `@fragment fn main(...) -> <ReturnType> { ... }`; the outputs are the
    // @location fields of the return struct (or a single inline `-> @location(0) vec4`).
    // We deliberately parse only the RETURN type so fragment *inputs* (interpolated
    // varyings, also @location) are excluded. The result drives the pipeline's color
    // targets and the render pass' color attachments.
    static void ParseWgslFragmentOutputs(const std::string& wgsl, std::vector<Uint32>& out) {
        const size_t fp = wgsl.find("@fragment");
        if (fp == std::string::npos) {
            return;
        }
        const size_t arrow = wgsl.find("->", fp);
        const size_t body = wgsl.find('{', fp);
        if (arrow == std::string::npos || body == std::string::npos || arrow > body) {
            return; // no return type (fragment writes nothing)
        }
        auto readLocation = [](const std::string& s, size_t at, Uint32& value) -> bool {
            static const std::string tok = "@location(";
            if (s.compare(at, tok.size(), tok) != 0) return false;
            size_t ns = at + tok.size();
            Uint32 n = 0;
            bool got = false;
            while (ns < s.size() && s[ns] >= '0' && s[ns] <= '9') {
                n = n * 10 + static_cast<Uint32>(s[ns] - '0');
                ++ns;
                got = true;
            }
            if (got) value = n;
            return got;
        };
        // Return type token sits between "->" and "{".
        size_t rs = arrow + 2;
        while (rs < body && (wgsl[rs] == ' ' || wgsl[rs] == '\t' || wgsl[rs] == '\n')) ++rs;
        Uint32 inlineLoc = 0;
        if (readLocation(wgsl, rs, inlineLoc)) {
            out.push_back(inlineLoc); // single inline output: `-> @location(0) vec4<f32>`
            return;
        }
        size_t re = rs;
        while (re < body && (std::isalnum(static_cast<unsigned char>(wgsl[re])) || wgsl[re] == '_')) ++re;
        const std::string retType = wgsl.substr(rs, re - rs);
        if (retType.empty()) {
            return;
        }
        // Find `struct <retType> {` ... `}` and collect its @location fields.
        const std::string decl = "struct " + retType;
        const size_t sp = wgsl.find(decl);
        if (sp == std::string::npos) {
            return;
        }
        const size_t open = wgsl.find('{', sp);
        const size_t close = (open == std::string::npos) ? std::string::npos : wgsl.find('}', open);
        if (open == std::string::npos || close == std::string::npos) {
            return;
        }
        static const std::string tok = "@location(";
        for (size_t p = wgsl.find(tok, open); p != std::string::npos && p < close;
             p = wgsl.find(tok, p + tok.size())) {
            Uint32 loc = 0;
            if (readLocation(wgsl, p, loc)) {
                out.push_back(loc);
            }
        }
    }

    // Texture unit a sampler uniform resolves to (its glUniform1i value); 0 if unknown.
    static Int ResolveSamplerUnit(const MG_State::GLState::ProgramObject& program, const String& glName) {
        const Int loc = program.GetUniformLocation(glName);
        if (loc < 0) return 0;
        const Int u = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(loc));
        return u >= 0 ? u : 0;
    }

    // Does the Texture2D bound to that sampler's unit carry a GL depth internal format?
    // Const/state-only, so it's safe from ComputePipelineKey and the pipeline builder.
    static Bool BoundTextureIsDepth(const MG_State::GLState::ProgramObject& program, const String& glName) {
        const Int unit = ResolveSamplerUnit(program, glName);
        auto& tu = MG_State::pGLContext->GetTextureUnitObject(unit);
        auto texObj = tu.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
        if (!texObj) return false;
        return ToDepthFormat(texObj->GetFormat()) != WGPUTextureFormat_Undefined;
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
            // Offset this stage's bindings into a disjoint range so vertex/fragment
            // resources never collide in WebGPU's shared @group(0).
            const std::string code =
                OffsetWgslBindings(wgsl.wgsl, StageBindingOffset(shaders[i]->GetShaderStage()));
            const std::string code2 = RemapClipSpace(code, shaders[i]->GetShaderStage());
            // ?dumpwgsl=N (shell) prints the final WGSL of the replay program whose
            // GL name is N, for transpile debugging.
            static const Uint32 dumpWgslProg = static_cast<Uint32>(EM_ASM_INT({
                return (typeof Module !== 'undefined' && Module['dumpWgsl']) ? Module['dumpWgsl'] : 0;
            }));
            if (dumpWgslProg != 0 && program.GetExternalIndex() == dumpWgslProg) {
                std::printf("[webgpu] WGSL prog=%u stage=%d begin\n%s\n[webgpu] WGSL prog=%u stage=%d end\n",
                            program.GetExternalIndex(), static_cast<int>(shaders[i]->GetShaderStage()),
                            code2.c_str(), program.GetExternalIndex(),
                            static_cast<int>(shaders[i]->GetShaderStage()));
            }
            // Parse the vertex stage's @location inputs so the pipeline can declare all
            // of them (WebGPU rejects a pipeline whose shader uses a location the vertex
            // state doesn't provide; that invalid pipeline then poisons the whole
            // command buffer).
            if (shaders[i]->GetShaderStage() == ShaderStage::Vertex) {
                std::vector<std::pair<Uint32, int>> locs;
                ParseWgslVertexLocations(code2, locs);
                for (const auto& [loc, baseType] : locs) {
                    prog.vertexLocations.push_back(VertexInput{loc, baseType != 0, baseType == 2});
                }
            } else if (shaders[i]->GetShaderStage() == ShaderStage::Fragment) {
                // Fragment output @location set -> the pipeline's color-target layout.
                ParseWgslFragmentOutputs(code2, prog.fragmentOutputs);
            }
            // The WGSL is the exact module interface; parse it for the bind group
            // layout (rather than SPIRV-Reflect, which can't see tint dropping the
            // sampler half of a texelFetch-only combined sampler).
            std::vector<ParsedWgslResource> parsed;
            ParseWgslResources(code, parsed);
            for (const auto& r : parsed) {
                // Skip resources declared but not statically used: WebGPU's auto layout
                // omits them, and binding them would invalidate the whole command buffer.
                if (CountIdentifierOccurrences(code, r.varName) <= 1) {
                    continue;
                }
                if (r.kind == ParsedWgslResource::Kind::Ubo) {
                    if (r.typeName.find("MGL_GLOBAL_UBO") == String::npos) {
                        continue; // only the default-block UBO is driven by glUniform*
                    }
                    if (std::find(prog.uboBindings.begin(), prog.uboBindings.end(), r.binding) ==
                        prog.uboBindings.end()) {
                        prog.uboBindings.push_back(r.binding);
                    }
                } else {
                    ResourceRef ref;
                    ref.binding = r.binding;
                    ref.kind = (r.kind == ParsedWgslResource::Kind::Texture) ? ResourceRef::Kind::Texture
                                                                             : ResourceRef::Kind::Sampler;
                    ref.glName = BaseUniformName(r.varName);
                    ref.wgslDepth = r.depth;
                    ref.wgslComparison = r.comparison;
                    prog.resources.push_back(ref);
                }
            }
            WGPUShaderModule module = MakeShaderModule(code2.c_str());
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
        if (!prog.uboBindings.empty()) {
            prog.globalUboSize = program.GetUBOSize();
        }
        auto [ins, ok] = m_programCache.emplace(&program, prog);
        return &ins->second;
    }

    Uint64 WebGPURenderer::ComputePipelineKey(const MG_State::GLState::ProgramObject& program,
                                              const MG_State::GLState::VertexArrayObject& vao,
                                              GLenum mode, const WgpuProgram* prog) const {
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
        // Current draw target(s): per-slot color format (holes included) + slot count +
        // whether there is a depth attachment (all baked into the pipeline by WebGPU).
        mix(static_cast<Uint64>(m_curColorCount));
        for (Uint i = 0; i < m_curColorCount; ++i) {
            mix((static_cast<Uint64>(m_curColor[i].format) << 1) | (m_curColor[i].view ? 1u : 0u));
        }
        mix(m_curDepthView != nullptr ? 1 : 0);
        mix(static_cast<Uint64>(m_curDepthFormat));
        // Whether each sampled texture resolves to a depth format decides the bind-group
        // layout (float vs unfilterable-float/depth sample type), which is baked into the
        // pipeline — so distinct depth/non-depth bindings need distinct cached pipelines.
        if (prog) {
            for (const auto& r : prog->resources) {
                if (r.kind == ResourceRef::Kind::Texture) {
                    mix(BoundTextureIsDepth(program, r.glName) ? 1u : 0u);
                }
            }
        }
        return key;
    }

    // A vertex-buffer group: one bound WebGPU vertex buffer feeding one or more attributes.
    // Attributes that share a GL buffer + array stride + step mode and are 4-aligned are
    // interleaved into a single group (one bound buffer, attributes at their real offsets);
    // int->float-converted and non-4-aligned (deinterleaved) attributes each get their own
    // group. Grouping keeps the vertex-buffer count within WebGPU's 8-slot cap for shaders
    // that declare many attributes (which would otherwise be skipped).
    struct VGroupAttr {
        Uint32 location;
        WGPUVertexFormat format;
        uint64_t offset; // byte offset within arrayStride
    };
    struct VGroup {
        enum Kind { Aligned, Convert, Deinterleave } kind = Aligned;
        WGPUVertexStepMode stepMode = WGPUVertexStepMode_Vertex;
        uint64_t arrayStride = 0;
        std::vector<VGroupAttr> attrs;
        MG_State::GLState::BufferObject* buffer = nullptr; // GL source buffer (identity + whole-bind)
        // Buffer-build params for Convert / Deinterleave singletons:
        Uint32 srcOffset = 0, glStride = 0, compByteSize = 0, size = 0, attrBytes = 0;
        Bool isSigned = false;
    };

    // Shared by GetOrCreatePipeline (builds layouts) and BeginDrawPass (binds buffers) so
    // the two stay 1:1 in count and order. Mirrors the attribute filtering / interleave /
    // convert / deinterleave decisions exactly once. shaderExpects(loc) returns the shader's
    // base type at a @location (-1 none, 0 float, 1 i32, 2 u32).
    template <typename ShaderExpects>
    static std::vector<VGroup> ComputeVertexGroups(const MG_State::GLState::VertexArrayObject& vao,
                                                   ShaderExpects&& shaderExpects) {
        std::vector<VGroup> groups;
        for (Uint i = 0; i < 16; ++i) {
            if (!vao.IsAttributeEnabled(i)) continue;
            const auto& a = vao.GetAttribute(i);
            const WGPUVertexFormat vf = ToVertexFormat(a.Type, a.Size, a.Normalized, a.IsInteger);
            if (!a.Buffer || vf == WGPUVertexFormat_Force32) continue;
            const int exp = shaderExpects(i);
            const int baseVf = VertexFormatBaseType(vf);
            const Bool convert = (exp == 0 && baseVf != 0 && a.Size >= 1 && a.Size <= 4);
            if (exp >= 0 && baseVf != exp && !convert) continue; // base-type mismatch -> dummy
            const Uint32 attrBytes = ComponentByteSize(a.Type) * a.Size;
            const Uint32 glStride = a.Stride ? static_cast<Uint32>(a.Stride) : attrBytes;
            const WGPUVertexStepMode stepMode =
                a.Divisor > 0 ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
            // Interleave into a shared group only when 4-aligned AND the attribute fits
            // within the stride (WebGPU needs offset%4==0, stride%4==0, offset+size<=stride).
            const Bool aligned = !convert && (a.Offset % 4u == 0u) && (glStride % 4u == 0u) &&
                                 (static_cast<Uint32>(a.Offset) + attrBytes <= glStride);
            if (aligned) {
                VGroup* dst = nullptr;
                for (auto& g : groups) {
                    if (g.kind == VGroup::Aligned && g.buffer == a.Buffer.get() &&
                        g.arrayStride == glStride && g.stepMode == stepMode) { dst = &g; break; }
                }
                if (!dst) {
                    groups.emplace_back();
                    dst = &groups.back();
                    dst->kind = VGroup::Aligned;
                    dst->stepMode = stepMode;
                    dst->arrayStride = glStride;
                    dst->buffer = a.Buffer.get();
                }
                dst->attrs.push_back(VGroupAttr{i, vf, static_cast<uint64_t>(a.Offset)});
                continue;
            }
            VGroup g;
            g.stepMode = stepMode;
            g.buffer = a.Buffer.get();
            g.srcOffset = static_cast<Uint32>(a.Offset);
            g.glStride = glStride;
            g.compByteSize = ComponentByteSize(a.Type);
            g.size = static_cast<Uint32>(a.Size);
            g.attrBytes = attrBytes;
            g.isSigned = (a.Type == DataType::Int8 || a.Type == DataType::Int16 ||
                          a.Type == DataType::Int32);
            if (convert) {
                g.kind = VGroup::Convert;
                g.arrayStride = static_cast<uint64_t>(a.Size) * 4u; // tight Float32x{size}
                g.attrs.push_back(VGroupAttr{i, FloatFormatForSize(a.Size), 0});
            } else {
                g.kind = VGroup::Deinterleave;
                g.arrayStride = static_cast<uint64_t>((attrBytes + 3u) & ~3u);
                g.attrs.push_back(VGroupAttr{i, vf, 0});
            }
            groups.push_back(std::move(g));
        }
        return groups;
    }

    const WebGPURenderer::WgpuPipeline*
    WebGPURenderer::GetOrCreatePipeline(MG_State::GLState::ProgramObject& program,
                                        const MG_State::GLState::VertexArrayObject& vao, GLenum mode) {
        const WgpuProgram* prog = GetOrCreateProgram(program);
        if (!prog) {
            return nullptr;
        }
        const Uint64 key = ComputePipelineKey(program, vao, mode, prog);
        if (auto it = m_pipelineCache.find(key); it != m_pipelineCache.end()) {
            return &it->second;
        }

        // The shader's base type at a @location (-1 none, 0 float, 1 i32, 2 u32). A VAO
        // attribute whose WGPU format base type doesn't match is dropped to a dummy of the
        // shader's type (no WebGPU format widens int->float without a converted buffer).
        auto shaderExpects = [&](Uint loc) -> int {
            for (const auto& vi : prog->vertexLocations) {
                if (vi.location == loc) return !vi.isInt ? 0 : (vi.isUnsigned ? 2 : 1);
            }
            return -1;
        };
        // Vertex-buffer groups (see ComputeVertexGroups): interleaved attributes sharing a
        // buffer collapse into one bound buffer, so many-attribute shaders stay within
        // WebGPU's 8-slot cap. The draw path walks the same groups in the same order.
        const std::vector<VGroup> groups = ComputeVertexGroups(vao, shaderExpects);

        // Flatten each group's attributes into one contiguous array (each layout points at
        // its group's slice), then append dummy attributes for shader @locations the VAO
        // doesn't supply — WebGPU requires every @location present in the vertex state, or
        // CreateRenderPipeline fails and the invalid pipeline poisons the command buffer.
        std::vector<WGPUVertexAttribute> attrs;
        attrs.reserve(prog->vertexLocations.size() + 4);
        Bool covered[64] = {};
        for (const auto& g : groups) {
            for (const auto& ga : g.attrs) {
                WGPUVertexAttribute va{};
                va.format = ga.format;
                va.offset = ga.offset;
                va.shaderLocation = ga.location;
                attrs.push_back(va);
                if (ga.location < 64) covered[ga.location] = true;
            }
        }
        // Dummy attributes (float/i32/u32 to match the shader's declared type) share one
        // constant-zero buffer; each gets a distinct 16-byte slot so ranges don't overlap.
        const SizeT firstDummy = attrs.size();
        Uint dummyCount = 0;
        for (const auto& vi : prog->vertexLocations) {
            if (vi.location < 64 && covered[vi.location]) continue;
            WGPUVertexAttribute va{};
            va.format = !vi.isInt ? WGPUVertexFormat_Float32x4
                        : (vi.isUnsigned ? WGPUVertexFormat_Uint32x4 : WGPUVertexFormat_Sint32x4);
            va.offset = static_cast<uint64_t>(dummyCount) * 16u;
            va.shaderLocation = vi.location;
            attrs.push_back(va);
            ++dummyCount;
            if (vi.location < 64) covered[vi.location] = true;
        }

        // One layout per group (each with its group's attributes), plus one shared layout
        // for all dummies (arrayStride 0 -> every vertex reads the same constant region).
        std::vector<WGPUVertexBufferLayout> layouts;
        layouts.reserve(groups.size() + 1);
        SizeT attrCursor = 0;
        for (const auto& g : groups) {
            WGPUVertexBufferLayout layout{};
            layout.stepMode = g.stepMode;
            layout.arrayStride = g.arrayStride;
            layout.attributeCount = g.attrs.size();
            layout.attributes = &attrs[attrCursor];
            attrCursor += g.attrs.size();
            layouts.push_back(layout);
        }
        if (dummyCount > 0) {
            WGPUVertexBufferLayout layout{};
            layout.stepMode = WGPUVertexStepMode_Vertex;
            layout.arrayStride = 0;
            layout.attributeCount = dummyCount;
            layout.attributes = &attrs[firstDummy];
            layouts.push_back(layout);
        }

        // WebGPU caps vertex buffers at 8 and attributes at 16; a pipeline exceeding either
        // becomes an error object that poisons the whole command buffer. Skip the draw
        // instead (now rare — only shaders with >16 distinct attributes or >8 buffers).
        if (layouts.size() > 8 || attrs.size() > 16) {
            static Bool warned = false;
            if (!warned) {
                warned = true;
                MGLOG_W("DirectWebGPU: draw skipped: %zu vertex buffers / %zu attributes exceed WebGPU "
                        "limits (8/16)", layouts.size(), attrs.size());
            }
            return nullptr;
        }

        auto* gl = MG_State::pGLContext.get();

        // Per-slot color targets, indexed by fragment output @location. WebGPU requires
        // a target exactly where the shader writes; unwritten slots below the max are
        // holes (format Undefined). The write mask / blend state are shared across all
        // targets (GL's per-buffer color mask / blend-i are not tracked yet).
        const auto cmask = gl->GetColorMask();
        const WGPUColorWriteMask writeMask =
            (cmask.r() ? WGPUColorWriteMask_Red : 0u) | (cmask.g() ? WGPUColorWriteMask_Green : 0u) |
            (cmask.b() ? WGPUColorWriteMask_Blue : 0u) | (cmask.a() ? WGPUColorWriteMask_Alpha : 0u);
        const Bool blendEnabled = gl->IsCapabilityEnabled(CapabilityInput::Blend);
        WGPUBlendState blend{};
        if (blendEnabled) {
            BlendFactor sR, dR, sA, dA;
            gl->GetBlendFunc(sR, dR, sA, dA);
            BlendEquation eC, eA;
            gl->GetBlendEquation(eC, eA);
            blend.color = {ToBlendOp(eC), ToBlendFactor(sR), ToBlendFactor(dR)};
            blend.alpha = {ToBlendOp(eA), ToBlendFactor(sA), ToBlendFactor(dA)};
        }

        Uint32 targetCount = 0;
        for (Uint32 loc : prog->fragmentOutputs) {
            targetCount = std::max(targetCount, loc + 1);
        }
        std::vector<WGPUColorTargetState> colorTargets(targetCount);
        std::vector<char> isOutput(targetCount, 0);
        for (Uint32 loc : prog->fragmentOutputs) {
            if (loc < targetCount) isOutput[loc] = 1;
        }
        for (Uint32 i = 0; i < targetCount; ++i) {
            WGPUColorTargetState& ct = colorTargets[i];
            ct = {};
            if (!isOutput[i]) {
                ct.format = WGPUTextureFormat_Undefined; // hole (shader doesn't write it)
                continue;
            }
            // The shader writes @location(i): a resolved attachment must exist there.
            if (i >= m_curColorCount || !m_curColor[i].view ||
                m_curColor[i].format == WGPUTextureFormat_Undefined) {
                MGLOG_E("DirectWebGPU: fragment writes location %u but draw buffer %u is "
                        "unbound/unsupported; skipping draw", i, i);
                return nullptr;
            }
            ct.format = m_curColor[i].format;
            ct.writeMask = writeMask;
            if (blendEnabled && IsBlendableFormat(ct.format)) {
                ct.blend = &blend;
            }
        }
        // A render pipeline needs at least one color target or a depth-stencil target.
        if (targetCount == 0 && m_curDepthView == nullptr) {
            return nullptr;
        }

        WGPUFragmentState fragment{};
        fragment.module = prog->fragment;
        fragment.entryPoint = Wgpu::View("main");
        fragment.targetCount = targetCount;
        fragment.targets = targetCount ? colorTargets.data() : nullptr;

        // Depth-stencil: declared only if the current target has a depth attachment.
        // depthWrite only when the test is on (GL doesn't write depth with the test off).
        const Bool hasDepth = (m_curDepthView != nullptr);
        const Bool depthTest = gl->IsCapabilityEnabled(CapabilityInput::DepthTest);
        WGPUDepthStencilState depthState{};
        depthState.format = m_curDepthFormat;
        depthState.depthWriteEnabled = (depthTest && gl->GetDepthMask())
                                           ? WGPUOptionalBool_True
                                           : WGPUOptionalBool_False;
        depthState.depthCompare = depthTest ? ToCompareFunc(gl->GetDepthFunc()) : WGPUCompareFunction_Always;
        depthState.stencilFront.compare = WGPUCompareFunction_Always;
        depthState.stencilBack.compare = WGPUCompareFunction_Always;

        // Bind-group layout. Tint's auto layout suffices unless a plain sampler2D binds a
        // depth-format texture: auto types it "float", which WebGPU won't let a depth
        // texture bind to. Then build an explicit layout with the right per-binding types
        // (depth / unfilterable-float sample types, comparison / non-filtering samplers).
        Bool needsExplicit = false;
        if (prog->HasResources()) {
            for (const auto& r : prog->resources) {
                if (r.kind == ResourceRef::Kind::Texture && !r.wgslDepth &&
                    BoundTextureIsDepth(program, r.glName)) {
                    needsExplicit = true;
                    break;
                }
            }
        }
        WGPUBindGroupLayout explicitBgl = nullptr;
        WGPUPipelineLayout explicitPl = nullptr;
        if (needsExplicit) {
            auto visOf = [](Uint32 binding) -> WGPUShaderStage {
                return binding >= kStageBindingStride ? WGPUShaderStage_Fragment : WGPUShaderStage_Vertex;
            };
            std::vector<WGPUBindGroupLayoutEntry> bgle;
            for (Uint32 b : prog->uboBindings) {
                WGPUBindGroupLayoutEntry e{};
                e.binding = b;
                e.visibility = visOf(b);
                e.buffer.type = WGPUBufferBindingType_Uniform;
                bgle.push_back(e);
            }
            for (const auto& r : prog->resources) {
                WGPUBindGroupLayoutEntry e{};
                e.binding = r.binding;
                e.visibility = visOf(r.binding);
                if (r.kind == ResourceRef::Kind::Texture) {
                    e.texture.viewDimension = WGPUTextureViewDimension_2D;
                    e.texture.multisampled = 0;
                    e.texture.sampleType = r.wgslDepth ? WGPUTextureSampleType_Depth
                                           : BoundTextureIsDepth(program, r.glName)
                                               ? WGPUTextureSampleType_UnfilterableFloat
                                               : WGPUTextureSampleType_Float;
                } else {
                    e.sampler.type = r.wgslComparison           ? WGPUSamplerBindingType_Comparison
                                     : BoundTextureIsDepth(program, r.glName)
                                         ? WGPUSamplerBindingType_NonFiltering
                                         : WGPUSamplerBindingType_Filtering;
                }
                bgle.push_back(e);
            }
            WGPUBindGroupLayoutDescriptor bgld{};
            bgld.entryCount = bgle.size();
            bgld.entries = bgle.empty() ? nullptr : bgle.data();
            explicitBgl = wgpuDeviceCreateBindGroupLayout(m_device, &bgld);
            WGPUPipelineLayoutDescriptor pld{};
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &explicitBgl;
            explicitPl = wgpuDeviceCreatePipelineLayout(m_device, &pld);
        }

        WGPURenderPipelineDescriptor desc{};
        desc.layout = explicitPl; // null => tint's auto layout
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
            if (explicitPl) wgpuPipelineLayoutRelease(explicitPl);
            if (explicitBgl) wgpuBindGroupLayoutRelease(explicitBgl);
            return nullptr;
        }
        WgpuPipeline entry;
        entry.pipeline = pipeline;
        entry.dummyVertexSlots = dummyCount > 0 ? 1 : 0; // all dummies share one buffer
        // group0Layout is what per-draw bind groups are built against: either our explicit
        // layout (owned here) or tint's auto layout fetched from the pipeline.
        if (needsExplicit) {
            entry.pipelineLayout = explicitPl;
            entry.group0Layout = explicitBgl;
        } else if (prog->HasResources()) {
            entry.group0Layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
        }
        auto [ins, ok] = m_pipelineCache.emplace(key, entry);
        return &ins->second;
    }

    // queueWriteBuffer requires a 4-byte-multiple size; write the aligned prefix
    // directly and any 1-3 tail bytes via a zero-padded 4-byte chunk (the buffer
    // allocation is rounded up to 4, so the pad lands in slack space). Writing
    // size & ~3 alone would silently drop the tail (e.g. the last index of an
    // odd-count 16-bit index buffer).
    static void WriteBufferPadded(WGPUQueue queue, WGPUBuffer buf, const void* data, SizeT size) {
        const SizeT aligned = size & ~SizeT(3);
        if (aligned > 0) {
            wgpuQueueWriteBuffer(queue, buf, 0, data, aligned);
        }
        if (aligned != size) {
            Uint8 tail[4] = {};
            std::memcpy(tail, static_cast<const Uint8*>(data) + aligned, size - aligned);
            wgpuQueueWriteBuffer(queue, buf, aligned, tail, sizeof(tail));
        }
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
                cb.lastUseSerial = m_flushSerial;
                return cb.buffer; // up to date
            }
            if (cb.buffer && cb.size == allocSize && data) {
                // Same size, new contents -> re-upload in place. queueWriteBuffer
                // executes at submit time (before the whole pending command buffer),
                // so if a recorded-but-unsubmitted draw uses this buffer, submit it
                // first — else that draw would read the NEW contents. (Callers must
                // therefore resolve buffers before opening a render pass.)
                if (cb.lastUseSerial == m_flushSerial) {
                    FlushFrame();
                }
                WriteBufferPadded(m_queue, cb.buffer, data->data(), size);
                cb.serial = serial;
                cb.lastUseSerial = m_flushSerial;
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
        WriteBufferPadded(m_queue, buf, data->data(), size);
        cache.emplace(&buffer, WgpuBuffer{buf, serial, allocSize, m_flushSerial});
        return buf;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateVertexBuffer(MG_State::GLState::BufferObject& buffer) {
        return GetOrCreateBuffer(m_vertexBufferCache, buffer, WGPUBufferUsage_Vertex);
    }

    WGPUBuffer WebGPURenderer::GetOrCreateDeinterleavedBuffer(MG_State::GLState::BufferObject& buffer,
                                                              Uint32 offsetBytes, Uint32 strideBytes,
                                                              Uint32 attrBytes, Uint32 alignedStride) {
        const Uint64 serial = buffer.GetChangeSerial();
        const SizeT size = buffer.GetSize();
        const auto& data = buffer.GetDataReadOnly();
        if (strideBytes == 0 || attrBytes == 0 || alignedStride == 0 || !data ||
            size < static_cast<SizeT>(offsetBytes) + attrBytes) {
            return nullptr;
        }
        // Vertex v's attribute occupies [offsetBytes + v*stride, +attrBytes]; find how many fit.
        const SizeT vtxCount = (size - offsetBytes - attrBytes) / strideBytes + 1;
        const Uint64 newSize = static_cast<Uint64>(vtxCount) * alignedStride;
        // Cache key: FNV-1a of (buffer pointer, offset, stride, attrBytes).
        Uint64 keyHash = 1469598103934665603ull;
        auto mixByte = [&keyHash](Uint8 b) { keyHash ^= b; keyHash *= 1099511628211ull; };
        const auto ptrVal = static_cast<Uint64>(reinterpret_cast<SizeT>(&buffer));
        for (int i = 0; i < 8; ++i) mixByte(static_cast<Uint8>(ptrVal >> (i * 8)));
        for (int i = 0; i < 4; ++i) mixByte(static_cast<Uint8>(offsetBytes >> (i * 8)));
        for (int i = 0; i < 4; ++i) mixByte(static_cast<Uint8>(strideBytes >> (i * 8)));
        for (int i = 0; i < 4; ++i) mixByte(static_cast<Uint8>(attrBytes >> (i * 8)));

        auto build = [&](WGPUBuffer buf) {
            std::vector<Uint8> packed(newSize, 0);
            const Uint8* src = static_cast<const Uint8*>(data->data());
            for (SizeT v = 0; v < vtxCount; ++v) {
                std::memcpy(&packed[v * alignedStride], &src[offsetBytes + v * strideBytes], attrBytes);
            }
            wgpuQueueWriteBuffer(m_queue, buf, 0, packed.data(), packed.size());
        };

        auto it = m_repackedVertexBufferCache.find(keyHash);
        if (it != m_repackedVertexBufferCache.end()) {
            WgpuBuffer& cb = it->second;
            if (cb.serial == serial && cb.size == newSize) {
                cb.lastUseSerial = m_flushSerial;
                return cb.buffer; // up to date
            }
            if (cb.buffer && cb.size == newSize) {
                // Same size, new contents -> re-upload. queueWriteBuffer lands at submit
                // time, so submit any recorded draws still using it first (see WgpuBuffer).
                if (cb.lastUseSerial == m_flushSerial) {
                    FlushFrame();
                }
                build(cb.buffer);
                cb.serial = serial;
                cb.lastUseSerial = m_flushSerial;
                return cb.buffer;
            }
            if (cb.buffer) wgpuBufferRelease(cb.buffer);
            m_repackedVertexBufferCache.erase(it);
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = newSize;
        WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!buf) {
            return nullptr;
        }
        build(buf);
        m_repackedVertexBufferCache.emplace(keyHash, WgpuBuffer{buf, serial, newSize, m_flushSerial});
        return buf;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateFloatConvertedBuffer(MG_State::GLState::BufferObject& buffer,
                                                               Uint32 offsetBytes, Uint32 strideBytes,
                                                               Uint32 componentBytes, Bool isSigned,
                                                               Uint32 count) {
        const Uint64 serial = buffer.GetChangeSerial();
        const SizeT size = buffer.GetSize();
        const auto& data = buffer.GetDataReadOnly();
        const Uint32 attrBytes = componentBytes * count;
        if (componentBytes == 0 || count == 0 || count > 4 || strideBytes == 0 || !data ||
            size < static_cast<SizeT>(offsetBytes) + attrBytes) {
            return nullptr;
        }
        const SizeT vtxCount = (size - offsetBytes - attrBytes) / strideBytes + 1;
        const Uint32 outStride = count * 4u; // Float32x{count}
        const Uint64 newSize = static_cast<Uint64>(vtxCount) * outStride;
        // Cache key: FNV-1a of a "converted" marker + (buffer, offset, stride, comp, signed, count).
        Uint64 keyHash = 1469598103934665603ull;
        auto mixByte = [&keyHash](Uint8 b) { keyHash ^= b; keyHash *= 1099511628211ull; };
        mixByte(0xC0); // distinguishes converted from plain deinterleaved entries
        const auto ptrVal = static_cast<Uint64>(reinterpret_cast<SizeT>(&buffer));
        for (int i = 0; i < 8; ++i) mixByte(static_cast<Uint8>(ptrVal >> (i * 8)));
        for (int i = 0; i < 4; ++i) mixByte(static_cast<Uint8>(offsetBytes >> (i * 8)));
        for (int i = 0; i < 4; ++i) mixByte(static_cast<Uint8>(strideBytes >> (i * 8)));
        mixByte(static_cast<Uint8>(componentBytes));
        mixByte(static_cast<Uint8>(isSigned ? 1 : 0));
        mixByte(static_cast<Uint8>(count));

        auto readComp = [&](const Uint8* p) -> float {
            if (componentBytes == 1) {
                return isSigned ? static_cast<float>(static_cast<int8_t>(p[0]))
                                : static_cast<float>(static_cast<uint8_t>(p[0]));
            }
            if (componentBytes == 2) {
                const uint16_t u = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
                return isSigned ? static_cast<float>(static_cast<int16_t>(u)) : static_cast<float>(u);
            }
            const uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            return isSigned ? static_cast<float>(static_cast<int32_t>(u)) : static_cast<float>(u);
        };
        auto build = [&](WGPUBuffer buf) {
            std::vector<float> out(static_cast<SizeT>(vtxCount) * count, 0.0f);
            const Uint8* src = static_cast<const Uint8*>(data->data());
            for (SizeT v = 0; v < vtxCount; ++v) {
                for (Uint32 c = 0; c < count; ++c) {
                    out[v * count + c] = readComp(&src[offsetBytes + v * strideBytes + c * componentBytes]);
                }
            }
            wgpuQueueWriteBuffer(m_queue, buf, 0, out.data(), out.size() * sizeof(float));
        };

        auto it = m_repackedVertexBufferCache.find(keyHash);
        if (it != m_repackedVertexBufferCache.end()) {
            WgpuBuffer& cb = it->second;
            if (cb.serial == serial && cb.size == newSize) {
                cb.lastUseSerial = m_flushSerial;
                return cb.buffer;
            }
            if (cb.buffer && cb.size == newSize) {
                if (cb.lastUseSerial == m_flushSerial) {
                    FlushFrame();
                }
                build(cb.buffer);
                cb.serial = serial;
                cb.lastUseSerial = m_flushSerial;
                return cb.buffer;
            }
            if (cb.buffer) wgpuBufferRelease(cb.buffer);
            m_repackedVertexBufferCache.erase(it);
        }
        WGPUBufferDescriptor bd{};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = newSize;
        WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
        if (!buf) {
            return nullptr;
        }
        build(buf);
        m_repackedVertexBufferCache.emplace(keyHash, WgpuBuffer{buf, serial, newSize, m_flushSerial});
        return buf;
    }

    WGPUBuffer WebGPURenderer::GetOrCreateIndexBuffer(MG_State::GLState::BufferObject& buffer) {
        return GetOrCreateBuffer(m_indexBufferCache, buffer, WGPUBufferUsage_Index);
    }

    // A shared, constant (arrayStride 0) zero vertex buffer used to satisfy shader vertex
    // inputs the GL VAO doesn't provide (disabled / unsupported-format attributes).
    WGPUBuffer WebGPURenderer::GetDummyVertexBuffer() {
        if (!m_dummyVertexBuffer) {
            WGPUBufferDescriptor bd{};
            bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            // 16 distinct 16-byte (vec4) slots of zeros: all dummy attributes share this
            // one buffer (arrayStride 0), each reading its own slot (offset = index*16).
            bd.size = 16 * 16;
            m_dummyVertexBuffer = wgpuDeviceCreateBuffer(m_device, &bd);
            if (m_dummyVertexBuffer) {
                const Uint8 zeros[16 * 16] = {};
                wgpuQueueWriteBuffer(m_queue, m_dummyVertexBuffer, 0, zeros, sizeof(zeros));
            }
        }
        return m_dummyVertexBuffer;
    }

    const WebGPURenderer::WgpuTexture*
    WebGPURenderer::GetOrCreateTexture(MG_State::GLState::ITextureObject& texture) {
        // Depth-format textures (depthtex*/shadowtex*) take a separate path: a sampleable
        // depth texture with depth-aspect views, no color mip upload / sRGB handling.
        if (const WGPUTextureFormat depthFmt = ToDepthFormat(texture.GetFormat());
            depthFmt != WGPUTextureFormat_Undefined) {
            return GetOrCreateDepthTexture(texture, depthFmt);
        }
        const ColorFormatInfo info = ToColorFormat(texture.GetFormat());
        const WGPUTextureFormat fmt = info.format;
        if (fmt == WGPUTextureFormat_Undefined) {
            std::printf("[webgpu] unsupported sampled texture format enum=%d\n", static_cast<int>(texture.GetFormat()));
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
        const Uint32 mipLevelCount = CountValidMipLevels(*mip, target);
        if (mipLevelCount == 0) {
            return nullptr;
        }
        const auto& levelRange = texture.GetLevelRange();
        const Uint32 baseMipLevel = std::min(levelRange.x(), mipLevelCount - 1u);
        const Uint32 maxMipLevel = std::min(levelRange.y(), mipLevelCount - 1u);
        const Uint32 viewMipLevelCount = maxMipLevel >= baseMipLevel ? (maxMipLevel - baseMipLevel + 1u) : 1u;
        const Uint16 textureParamsVersion = texture.GetTextureParamsVersion();

        // Reuse the cached GPU texture unless its content is dirty (glTexImage2D /
        // glTexSubImage2D mark it so). A size/format change forces a recreate.
        auto it = m_textureCache.find(&texture);
        if (it != m_textureCache.end()) {
            WgpuTexture& cached = it->second;
            Bool dirty = false;
            for (Uint32 level = 0; level < mipLevelCount; ++level) {
                if (mip->IsStorageDirty(target, level)) {
                    dirty = true;
                    break;
                }
            }
            const Bool viewDirty = cached.textureParamsVersion != textureParamsVersion ||
                                   cached.viewBaseMipLevel != baseMipLevel ||
                                   cached.viewMipLevelCount != viewMipLevelCount;
            if (!dirty && !viewDirty) {
                cached.lastUseSerial = m_flushSerial;
                return &cached; // up to date
            }
            if (cached.width != w || cached.height != h || cached.format != fmt || cached.mipLevelCount != mipLevelCount) {
                if (cached.attachmentSrgbView) wgpuTextureViewRelease(cached.attachmentSrgbView);
                if (cached.attachmentView) wgpuTextureViewRelease(cached.attachmentView);
                if (cached.view) wgpuTextureViewRelease(cached.view);
                if (cached.texture) wgpuTextureRelease(cached.texture);
                m_textureCache.erase(it);
            } else {
                if (dirty) {
                    // queueWriteTexture executes at submit time; if recorded draws
                    // sample (or render into) this texture, submit them before the
                    // re-upload or they would see the new texels.
                    if (cached.lastUseSerial == m_flushSerial) {
                        FlushFrame();
                    }
                    UploadTextureLevels(cached.texture, *mip, info.bytesPerTexel,
                                        info.srcBytesPerTexel ? info.srcBytesPerTexel : info.bytesPerTexel);
                }
                if (viewDirty) {
                    if (cached.view) {
                        wgpuTextureViewRelease(cached.view);
                    }
                    WGPUTextureViewDescriptor vd{};
                    vd.format = fmt;
                    vd.dimension = WGPUTextureViewDimension_2D;
                    vd.baseMipLevel = baseMipLevel;
                    vd.mipLevelCount = viewMipLevelCount;
                    vd.baseArrayLayer = 0;
                    vd.arrayLayerCount = 1;
                    vd.aspect = WGPUTextureAspect_All;
                    cached.view = wgpuTextureCreateView(cached.texture, &vd);
                    cached.viewBaseMipLevel = baseMipLevel;
                    cached.viewMipLevelCount = viewMipLevelCount;
                    cached.textureParamsVersion = textureParamsVersion;
                }
                cached.lastUseSerial = m_flushSerial;
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
        td.mipLevelCount = mipLevelCount;
        td.sampleCount = 1;
        // An sRGB texture also gets a linear (UNORM) view: it is the render-attachment
        // view while GL_FRAMEBUFFER_SRGB is disabled (desktop GL only encodes on write
        // with it enabled). UNORM textures need no extra view format — GL never
        // sRGB-encodes into a linear attachment.
        const WGPUTextureFormat linearViewFormat = ToLinearViewFormat(fmt);
        if (linearViewFormat != fmt) {
            td.viewFormatCount = 1;
            td.viewFormats = &linearViewFormat;
        }
        WGPUTexture tex = wgpuDeviceCreateTexture(m_device, &td);
        if (!tex) {
            return nullptr;
        }
        // Upload level-0 CPU data if present; FBO attachments allocated without data
        // (glTexImage2D NULL) just stay zero-initialized and get rendered into.
        UploadTextureLevels(tex, *mip, info.bytesPerTexel,
                            info.srcBytesPerTexel ? info.srcBytesPerTexel : info.bytesPerTexel);

        WgpuTexture wt;
        wt.texture = tex;
        WGPUTextureViewDescriptor vd{};
        vd.format = fmt;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = baseMipLevel;
        vd.mipLevelCount = viewMipLevelCount;
        vd.baseArrayLayer = 0;
        vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        wt.view = wgpuTextureCreateView(tex, &vd);
        WGPUTextureViewDescriptor attachmentDesc = vd;
        attachmentDesc.baseMipLevel = 0;
        attachmentDesc.mipLevelCount = 1;
        attachmentDesc.format = linearViewFormat;
        wt.attachmentView = wgpuTextureCreateView(tex, &attachmentDesc);
        // Only an sRGB-format texture can be sRGB-encoded on write (GL_FRAMEBUFFER_SRGB);
        // UNORM attachments never encode, so they get no sRGB view.
        if (fmt != linearViewFormat) {
            attachmentDesc.format = fmt;
            wt.attachmentSrgbView = wgpuTextureCreateView(tex, &attachmentDesc);
        }
        wt.width = w;
        wt.height = h;
        wt.mipLevelCount = mipLevelCount;
        wt.viewBaseMipLevel = baseMipLevel;
        wt.viewMipLevelCount = viewMipLevelCount;
        wt.textureParamsVersion = textureParamsVersion;
        wt.format = fmt;
        wt.lastUseSerial = m_flushSerial;
        auto [ins, ok] = m_textureCache.emplace(&texture, wt);
        return &ins->second;
    }

    const WebGPURenderer::WgpuTexture*
    WebGPURenderer::GetOrCreateDepthTexture(MG_State::GLState::ITextureObject& texture,
                                            WGPUTextureFormat depthFmt) {
        const IntVec3 dim = texture.GetBaseSize();
        if (dim.x() <= 0 || dim.y() <= 0) {
            return nullptr;
        }
        const Uint32 w = static_cast<Uint32>(dim.x());
        const Uint32 h = static_cast<Uint32>(dim.y());
        const Bool hasStencil = (depthFmt == WGPUTextureFormat_Depth24PlusStencil8);

        auto it = m_textureCache.find(&texture);
        if (it != m_textureCache.end()) {
            WgpuTexture& c = it->second;
            if (c.isDepth && c.width == w && c.height == h && c.format == depthFmt) {
                c.lastUseSerial = m_flushSerial;
                return &c; // depth targets carry no CPU data to re-upload
            }
            if (c.attachmentSrgbView) wgpuTextureViewRelease(c.attachmentSrgbView);
            if (c.attachmentView) wgpuTextureViewRelease(c.attachmentView);
            if (c.view) wgpuTextureViewRelease(c.view);
            if (c.texture) wgpuTextureRelease(c.texture);
            m_textureCache.erase(it);
        }

        WGPUTextureDescriptor td{};
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {w, h, 1};
        td.format = depthFmt;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        WGPUTexture tex = wgpuDeviceCreateTexture(m_device, &td);
        if (!tex) {
            return nullptr;
        }
        WgpuTexture wt;
        wt.texture = tex;
        // Sampled view: single (depth) aspect, required for texture bindings.
        WGPUTextureViewDescriptor vd{};
        vd.format = depthFmt;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = 0;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = 0;
        vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_DepthOnly;
        wt.view = wgpuTextureCreateView(tex, &vd);
        // Depth-stencil attachment view: include stencil aspect when present.
        WGPUTextureViewDescriptor ad = vd;
        ad.aspect = hasStencil ? WGPUTextureAspect_All : WGPUTextureAspect_DepthOnly;
        wt.attachmentView = wgpuTextureCreateView(tex, &ad);
        wt.width = w;
        wt.height = h;
        wt.mipLevelCount = 1;
        wt.viewBaseMipLevel = 0;
        wt.viewMipLevelCount = 1;
        wt.format = depthFmt;
        wt.isDepth = true;
        wt.lastUseSerial = m_flushSerial;
        auto [ins, ok] = m_textureCache.emplace(&texture, wt);
        return &ins->second;
    }

    void WebGPURenderer::UploadTextureLevels(WGPUTexture tex, MG_State::GLState::TextureObjectMipmap& mip,
                                             Uint32 bytesPerTexel, Uint32 srcBytesPerTexel) {
        const auto target = TextureUploadTarget::Texture2D;
        const Uint32 levelCount = CountValidMipLevels(mip, target);
        std::vector<Uint8> expandScratch;
        for (Uint32 level = 0; level < levelCount; ++level) {
            const IntVec3 dim = mip.GetMipmapTexelSize(target, level);
            const Uint32 w = static_cast<Uint32>(dim.x());
            const Uint32 h = static_cast<Uint32>(dim.y());
            const SizeT byteSize = mip.GetMipmapByteSize(target, level);
            void* pixels = mip.MapMipmapData(target, level);
            if (pixels && byteSize > 0) {
                const void* src = pixels;
                SizeT srcSize = byteSize;
                // WebGPU has no 3-channel format: expand an RGB(8) source into the RGBA8
                // texture, filling alpha with 0xFF. Only when the source really carries a
                // level's worth of 3-byte texels (render-target attachments have none).
                if (srcBytesPerTexel != 0 && srcBytesPerTexel != bytesPerTexel &&
                    srcBytesPerTexel == 3 && bytesPerTexel == 4 &&
                    byteSize >= static_cast<SizeT>(w) * h * 3) {
                    expandScratch.assign(static_cast<SizeT>(w) * h * 4, 0xFF);
                    const Uint8* s = static_cast<const Uint8*>(pixels);
                    for (SizeT px = 0; px < static_cast<SizeT>(w) * h; ++px) {
                        expandScratch[px * 4 + 0] = s[px * 3 + 0];
                        expandScratch[px * 4 + 1] = s[px * 3 + 1];
                        expandScratch[px * 4 + 2] = s[px * 3 + 2];
                        // alpha stays 0xFF
                    }
                    src = expandScratch.data();
                    srcSize = expandScratch.size();
                } else if (byteSize < static_cast<SizeT>(w) * h * bytesPerTexel) {
                    // Source smaller than the WGPU layout expects (e.g. an unexpanded
                    // packed format); skip rather than over-read.
                    mip.MarkStorageDirty(target, level, false);
                    continue;
                }
                WGPUTexelCopyTextureInfo dst{};
                dst.texture = tex;
                dst.mipLevel = level;
                WGPUTexelCopyBufferLayout dataLayout{};
                // Row pitch from the resolved WGPU format's texel size (RGBA8=4,
                // RGBA16F=8, RGBA32F=16, ...). queueWriteTexture has no 256B alignment
                // requirement (unlike buffer copies).
                dataLayout.bytesPerRow = w * bytesPerTexel;
                dataLayout.rowsPerImage = h;
                WGPUExtent3D ext{w, h, 1};
                wgpuQueueWriteTexture(m_queue, &dst, src, srcSize, &dataLayout, &ext);
            }
            mip.MarkStorageDirty(target, level, false);
        }
    }

    WGPUSampler WebGPURenderer::GetOrCreateSampler(const MG_State::GLState::SamplerObject& sampler,
                                                   Bool comparison, Bool forceNonFiltering) {
        WGPUSamplerDescriptor sd{};
        sd.addressModeU = ToWgpuAddressMode(sampler.GetWrapS());
        sd.addressModeV = ToWgpuAddressMode(sampler.GetWrapT());
        sd.addressModeW = ToWgpuAddressMode(sampler.GetWrapR());
        if (forceNonFiltering && !comparison) {
            // A depth texture sampled through a plain texture_2d<f32> is unfilterable-
            // float, so its sampler must be non-filtering (nearest, no interpolation).
            sd.magFilter = WGPUFilterMode_Nearest;
            sd.minFilter = WGPUFilterMode_Nearest;
            sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        } else {
            sd.magFilter = ToWgpuFilter(sampler.GetMagFilter());
            sd.minFilter = ToWgpuFilter(sampler.GetMinFilter());
            sd.mipmapFilter = ToWgpuMipmapFilter(sampler.GetMipmapMode());
        }
        // WebGPU requires 0 <= lodMinClamp <= lodMaxClamp; GL defaults (-1000..1000)
        // must be clamped. With no mipmaps yet this only affects validity.
        const Float maxLod = sampler.GetMipmapMode() == SamplerMipmapMode::None
                                 ? 0.0f
                                 : (sampler.GetMaxLod() < 0.0f ? 0.0f : sampler.GetMaxLod());
        const Float minLod = sampler.GetMinLod() < 0.0f ? 0.0f : sampler.GetMinLod();
        sd.lodMinClamp = minLod < maxLod ? minLod : maxLod;
        sd.lodMaxClamp = maxLod;
        sd.maxAnisotropy = 1;
        if (comparison) {
            // sampler2DShadow -> a comparison sampler (hardware PCF against a depth ref).
            using CF = SamplerCompareFunc;
            switch (sampler.GetSamplerCompareFunc()) {
            case CF::Never: sd.compare = WGPUCompareFunction_Never; break;
            case CF::Less: sd.compare = WGPUCompareFunction_Less; break;
            case CF::Equal: sd.compare = WGPUCompareFunction_Equal; break;
            case CF::Greater: sd.compare = WGPUCompareFunction_Greater; break;
            case CF::NotEqual: sd.compare = WGPUCompareFunction_NotEqual; break;
            case CF::GreaterEqual: sd.compare = WGPUCompareFunction_GreaterEqual; break;
            case CF::Always: sd.compare = WGPUCompareFunction_Always; break;
            case CF::LessEqual:
            default: sd.compare = WGPUCompareFunction_LessEqual; break;
            }
        }

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
        mix(static_cast<Uint32>(sd.compare)); // distinguishes comparison samplers
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

        // ---- Phase 1: resolve EVERY resource before opening the render pass. ----
        // Buffer/texture (re)uploads may FlushFrame() (submit the pending encoder,
        // see GetOrCreateBuffer/GetOrCreateTexture): that is illegal with an open
        // pass, and it also releases the per-frame UBO/bind-group lists — so all
        // flush-capable resolution happens first, and the fresh UBO + bind group are
        // created only after it.

        const WgpuProgram* prog = GetOrCreateProgram(program);
        if (!prog) {
            return nullptr;
        }
        // Same shader-base-type resolution as GetOrCreatePipeline, so the bound vertex
        // buffers stay 1:1 with the pipeline's vertex layouts.
        auto shaderExpects = [&](Uint loc) -> int {
            for (const auto& vi : prog->vertexLocations) {
                if (vi.location == loc) return !vi.isInt ? 0 : (vi.isUnsigned ? 2 : 1);
            }
            return -1;
        };

        // Vertex buffers, one per group (see ComputeVertexGroups) — same groups and order as
        // the pipeline's layouts, so slots stay 1:1. An interleaved (Aligned) group binds its
        // shared GL buffer once at offset 0 (each attribute carries its real byte offset);
        // Convert / Deinterleave groups get their own repacked buffer.
        std::vector<std::pair<WGPUBuffer, uint64_t>> vertexBuffers;
        for (const auto& g : ComputeVertexGroups(vao, shaderExpects)) {
            WGPUBuffer vb = nullptr;
            if (g.kind == VGroup::Convert) {
                vb = GetOrCreateFloatConvertedBuffer(*g.buffer, g.srcOffset, g.glStride, g.compByteSize,
                                                     g.isSigned, static_cast<int>(g.size));
            } else if (g.kind == VGroup::Deinterleave) {
                vb = GetOrCreateDeinterleavedBuffer(*g.buffer, g.srcOffset, g.glStride, g.attrBytes,
                                                    static_cast<Uint32>(g.arrayStride));
            } else {
                vb = GetOrCreateVertexBuffer(*g.buffer);
            }
            if (!vb) continue;
            vertexBuffers.emplace_back(vb, 0);
        }

        // Sampled textures + samplers, resolved to texture units by uniform name.
        // (A texelFetch-only texture has no companion sampler.)
        std::vector<WGPUBindGroupEntry> entries;
        const Bool wantResources = prog && prog->HasResources() && p->group0Layout;
        // Every resource the pipeline's auto layout expects must resolve; a missing one
        // (unsupported texture format, no texture bound) would leave the bind group short
        // an entry and invalidate the whole command buffer. Track and skip the draw if so.
        Bool resourcesOk = true;
        if (wantResources) {
            for (const auto& r : prog->resources) {
                Int unit = 0;
                const Int loc = program.GetUniformLocation(r.glName);
                if (loc >= 0) {
                    const Int u = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(loc));
                    if (u >= 0) {
                        unit = u;
                    } else if (g_loggedMissingSamplerUnits.insert(r.glName + "#unit").second) {
                        std::printf("[webgpu] sampler unit lookup failed for '%s' (loc=%d); defaulting to unit 0\n",
                                    r.glName.c_str(), loc);
                    }
                } else if (g_loggedMissingSamplerUnits.insert(r.glName + "#loc").second) {
                    std::printf("[webgpu] sampler uniform '%s' not found; defaulting to unit 0\n", r.glName.c_str());
                }
                auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                auto texObj = textureUnit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
                if (r.kind == ResourceRef::Kind::Texture) {
                    if (!texObj) {
                        if (g_loggedMissingSamplerUnits.insert(r.glName + "#tex").second) {
                            std::printf("[webgpu] sampler '%s' resolved to unit %d with no Texture2D bound\n",
                                        r.glName.c_str(), unit);
                        }
                        resourcesOk = false;
                        break;
                    }
                    const WgpuTexture* wt = GetOrCreateTexture(*texObj);
                    if (!wt) { resourcesOk = false; break; }
                    WGPUBindGroupEntry te{};
                    te.binding = r.binding;
                    te.textureView = wt->view;
                    entries.push_back(te);
                } else {
                    // Effective sampler: a bound GL sampler object (glBindSampler)
                    // overrides the texture object's own glTexParameter-set params.
                    const auto& samplerOverride = textureUnit.GetSamplerObject();
                    const auto& effectiveSampler =
                        samplerOverride ? samplerOverride : (texObj ? texObj->GetSamplerObject() : samplerOverride);
                    // sampler2DShadow -> comparison sampler; a plain sampler2D on a depth
                    // texture -> non-filtering (must match the unfilterable-float binding).
                    const Bool comparison = r.wgslComparison;
                    const Bool depthPaired =
                        texObj && ToDepthFormat(texObj->GetFormat()) != WGPUTextureFormat_Undefined;
                    WGPUSampler sampler = effectiveSampler
                                              ? GetOrCreateSampler(*effectiveSampler, comparison,
                                                                   depthPaired && !comparison)
                                              : nullptr;
                    if (!sampler) { resourcesOk = false; break; }
                    WGPUBindGroupEntry se{};
                    se.binding = r.binding;
                    se.sampler = sampler;
                    entries.push_back(se);
                }
            }
        }

        // A resource the pipeline layout requires couldn't be resolved -> the bind group
        // would be incomplete and poison the command buffer. Skip this draw instead (the
        // rest of the frame still renders). Common while backend format/depth support is
        // incomplete (e.g. an Iris pass sampling a depth/shadow or unsupported-format tex).
        if (wantResources && !resourcesOk) {
            return nullptr;
        }

        // ---- Phase 2: fresh per-draw UBO + bind group (no flushes past here). ----
        WGPUBindGroup bindGroup = nullptr;
        if (wantResources) {
            if (!prog->uboBindings.empty() && prog->globalUboSize > 0) {
                // Fresh per-draw UBO buffer with this draw's uniforms (see m_frameUboBuffers).
                // The same buffer is bound at every stage's UBO binding.
                const Uint64 uboSize = (static_cast<Uint64>(prog->globalUboSize) + 15u) & ~Uint64(15);
                WGPUBufferDescriptor bd{};
                bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                bd.size = uboSize;
                WGPUBuffer ubo = wgpuDeviceCreateBuffer(m_device, &bd);
                m_frameUboBuffers.push_back(ubo);
                const void* uboData = program.GetUBOData();
                const Uint sz = program.GetUBOSize();
                if (ubo && uboData && sz > 0) {
                    WriteBufferPadded(m_queue, ubo, uboData, sz);
                }
                for (Uint32 binding : prog->uboBindings) {
                    WGPUBindGroupEntry e{};
                    e.binding = binding;
                    e.buffer = ubo;
                    e.size = uboSize;
                    entries.push_back(e);
                }
            }
            if (!entries.empty()) {
                WGPUBindGroupDescriptor bgd{};
                bgd.layout = p->group0Layout;
                bgd.entryCount = entries.size();
                bgd.entries = entries.data();
                bindGroup = wgpuDeviceCreateBindGroup(m_device, &bgd);
                m_frameBindGroups.push_back(bindGroup);
            }
        }

        // ---- Phase 3: open the pass and record state. ----
        // Draw into a Load render pass so a prior glClear is preserved. Color attachments
        // must match the pipeline's color targets exactly: one per fragment output
        // @location, with holes (null) where the shader doesn't write. The depth
        // attachment is present only if the current target has one.
        if (!prog) {
            return nullptr;
        }
        Uint32 targetCount = 0;
        for (Uint32 loc : prog->fragmentOutputs) {
            targetCount = std::max(targetCount, loc + 1);
        }
        std::vector<char> isOutput(targetCount, 0);
        for (Uint32 loc : prog->fragmentOutputs) {
            if (loc < targetCount) isOutput[loc] = 1;
        }
        std::vector<WGPURenderPassColorAttachment> colors(targetCount);
        for (Uint32 i = 0; i < targetCount; ++i) {
            WGPURenderPassColorAttachment& a = colors[i];
            a = {};
            a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            if (isOutput[i] && i < m_curColorCount && m_curColor[i].view) {
                a.view = m_curColor[i].view;
                a.loadOp = WGPULoadOp_Load;
                a.storeOp = WGPUStoreOp_Store;
            } else {
                a.view = nullptr; // hole: loadOp/storeOp stay Undefined
            }
        }
        WGPURenderPassDepthStencilAttachment depth{};
        const Bool hasDepth = (m_curDepthView != nullptr);
        if (hasDepth) {
            depth.view = m_curDepthView;
            depth.depthLoadOp = WGPULoadOp_Load;
            depth.depthStoreOp = WGPUStoreOp_Store;
        }
        WGPURenderPassDescriptor rp{};
        rp.colorAttachmentCount = colors.size();
        rp.colorAttachments = colors.empty() ? nullptr : colors.data();
        rp.depthStencilAttachment = hasDepth ? &depth : nullptr;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, p->pipeline);

        auto* gl = MG_State::pGLContext.get();
        if (gl->IsCapabilityEnabled(CapabilityInput::Blend)) {
            const auto& blendColor = gl->GetBlendColor();
            const WGPUColor wgpuBlendColor = {static_cast<double>(blendColor[0]), static_cast<double>(blendColor[1]),
                                              static_cast<double>(blendColor[2]), static_cast<double>(blendColor[3])};
            wgpuRenderPassEncoderSetBlendConstant(pass, &wgpuBlendColor);
        }
        // Viewport. Render targets are stored in GL texel order (row 0 = GL window
        // bottom; see RemapClipSpace), so GL's bottom-left viewport rectangle maps to
        // texel rows directly with no flip. Only set when the app set a non-empty
        // viewport; otherwise WebGPU's default covers the whole target.
        const auto& vp = gl->GetViewport();
        if (vp.z() > 0 && vp.w() > 0) {
            wgpuRenderPassEncoderSetViewport(pass, static_cast<float>(vp.x()),
                                             static_cast<float>(vp.y()), static_cast<float>(vp.z()),
                                             static_cast<float>(vp.w()), 0.0f, 1.0f);
        }
        // Scissor: like the viewport, GL's bottom-left box maps to texel rows with no
        // flip (targets are stored in GL texel order). GL allows a scissor box
        // extending past the target, but WebGPU validates x+w <= target (and a
        // violation poisons the whole pass), so intersect with the target rect first.
        // A fully-clipped scissor still discards everything (zero-size rect).
        if (gl->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            const auto& sc = gl->GetScissorBox();
            Int32 sx = sc.x(), sy = sc.y(), sw = sc.z(), sh = sc.w();
            if (sx < 0) { sw += sx; sx = 0; }
            if (sy < 0) { sh += sy; sy = 0; }
            if (sx + sw > static_cast<Int32>(m_curWidth)) sw = static_cast<Int32>(m_curWidth) - sx;
            if (sy + sh > static_cast<Int32>(m_curHeight)) sh = static_cast<Int32>(m_curHeight) - sy;
            if (sw > 0 && sh > 0 && sx < static_cast<Int32>(m_curWidth) &&
                sy < static_cast<Int32>(m_curHeight)) {
                wgpuRenderPassEncoderSetScissorRect(pass, static_cast<Uint32>(sx),
                                                    static_cast<Uint32>(sy), static_cast<Uint32>(sw),
                                                    static_cast<Uint32>(sh));
            } else {
                wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, 0, 0);
            }
        }

        if (bindGroup) {
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        }

        Uint32 slot = 0;
        for (const auto& [vb, offset] : vertexBuffers) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, slot, vb, offset, WGPU_WHOLE_SIZE);
            ++slot;
        }
        // Dummy (constant zero) buffers for shader inputs the VAO didn't supply, in the
        // same order the pipeline appended their layouts.
        if (p->dummyVertexSlots > 0) {
            WGPUBuffer dummy = GetDummyVertexBuffer();
            for (Uint d = 0; d < p->dummyVertexSlots; ++d) {
                wgpuRenderPassEncoderSetVertexBuffer(pass, slot, dummy, 0, WGPU_WHOLE_SIZE);
                ++slot;
            }
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

        // IEEE half -> float (for float texture readback/diagnostics).
        float HalfToFloat(uint16_t h) {
            const uint32_t sign = (h & 0x8000u) << 16;
            const uint32_t exp = (h >> 10) & 0x1Fu;
            const uint32_t mant = h & 0x3FFu;
            uint32_t f;
            if (exp == 0) {
                if (mant == 0) {
                    f = sign; // +/-0
                } else {
                    // subnormal -> normalize
                    int e = -1;
                    uint32_t m = mant;
                    do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
                    m &= 0x3FFu;
                    f = sign | ((uint32_t)(127 - 15 - e) << 23) | (m << 13);
                }
            } else if (exp == 0x1Fu) {
                f = sign | 0x7F800000u | (mant << 13); // inf/nan
            } else {
                f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            }
            float out;
            std::memcpy(&out, &f, 4);
            return out;
        }
        Uint8 ToUnorm8(float v) {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<Uint8>(v * 255.0f + 0.5f);
        }
        // Byte size of a WGPU texture format we can read back (0 = unsupported).
        Uint32 ReadbackBytesPerTexel(WGPUTextureFormat f) {
            switch (f) {
            case WGPUTextureFormat_RGBA8Unorm:
            case WGPUTextureFormat_RGBA8UnormSrgb:
            case WGPUTextureFormat_BGRA8Unorm:
            case WGPUTextureFormat_BGRA8UnormSrgb:
            case WGPUTextureFormat_RG11B10Ufloat: return 4;
            case WGPUTextureFormat_RGBA16Float: return 8;
            case WGPUTextureFormat_RGBA32Float: return 16;
            default: return 0;
            }
        }
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
                                          WGPUTextureFormat srcFmt, GLenum dstFormat, Bool flipY, void* out) {
        if (m_readbackInFlight) {
            MGLOG_E("DirectWebGPU: reentrant readback while one is in flight; ignoring");
            return false;
        }
        const Uint32 bpt = ReadbackBytesPerTexel(srcFmt);
        if (bpt == 0) {
            MGLOG_E("DirectWebGPU: readback unsupported for texture format %d", static_cast<int>(srcFmt));
            return false;
        }
        const Uint32 bytesPerRow = AlignUp256(w * bpt); // WebGPU requires 256B row alignment
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
                const Bool srcIsBgra = (srcFmt == WGPUTextureFormat_BGRA8Unorm ||
                                        srcFmt == WGPUTextureFormat_BGRA8UnormSrgb);
                const Bool wantBgra = (dstFormat == GL_BGRA);
                const Bool swapRB = (srcIsBgra != wantBgra);
                const Bool isHalf = (srcFmt == WGPUTextureFormat_RGBA16Float);
                const Bool isFloat = (srcFmt == WGPUTextureFormat_RGBA32Float);
                const Bool isRG11B10 = (srcFmt == WGPUTextureFormat_RG11B10Ufloat);
                auto rd16 = [](const Uint8* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); };
                auto rd32f = [](const Uint8* p) {
                    uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
                    float f; std::memcpy(&f, &u, 4); return f;
                };
                for (Uint32 row = 0; row < h; ++row) {
                    const Uint64 srcRowIdx = flipY ? (h - 1 - row) : row;
                    const Uint8* srcRow = mapped + srcRowIdx * bytesPerRow;
                    Uint8* dstRow = dstBytes + static_cast<Uint64>(row) * w * 4u;
                    for (Uint32 px = 0; px < w; ++px) {
                        const Uint8* s = srcRow + static_cast<Uint64>(px) * bpt;
                        Uint8* d = dstRow + px * 4u;
                        if (isHalf) {
                            d[0] = ToUnorm8(HalfToFloat(rd16(s + 0)));
                            d[1] = ToUnorm8(HalfToFloat(rd16(s + 2)));
                            d[2] = ToUnorm8(HalfToFloat(rd16(s + 4)));
                            d[3] = ToUnorm8(HalfToFloat(rd16(s + 6)));
                        } else if (isFloat) {
                            d[0] = ToUnorm8(rd32f(s + 0));
                            d[1] = ToUnorm8(rd32f(s + 4));
                            d[2] = ToUnorm8(rd32f(s + 8));
                            d[3] = ToUnorm8(rd32f(s + 12));
                        } else if (isRG11B10) {
                            const uint32_t u = s[0] | (s[1] << 8) | (s[2] << 16) | (static_cast<uint32_t>(s[3]) << 24);
                            // R,G: 11-bit float (5e6m); B: 10-bit float (5e5m). Widen to half then decode.
                            const uint16_t rh = static_cast<uint16_t>((((u) & 0x7FFu) >> 6) << 10 | (((u) & 0x3Fu) << 4));
                            const uint16_t gh = static_cast<uint16_t>((((u >> 11) & 0x7FFu) >> 6) << 10 | (((u >> 11) & 0x3Fu) << 4));
                            const uint16_t bh = static_cast<uint16_t>((((u >> 22) & 0x3FFu) >> 5) << 10 | (((u >> 22) & 0x1Fu) << 5));
                            d[0] = ToUnorm8(HalfToFloat(rh));
                            d[1] = ToUnorm8(HalfToFloat(gh));
                            d[2] = ToUnorm8(HalfToFloat(bh));
                            d[3] = 255;
                        } else if (swapRB) {
                            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        } else {
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                        }
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
        // Flush pending draws so the offscreen target reflects them before the copy.
        if (m_frameActive) {
            FlushFrame();
        }
        WGPUTexture srcTex = nullptr;
        Uint32 srcW = 0, srcH = 0;
        WGPUTextureFormat srcFmt = WGPUTextureFormat_Undefined;
        if (!ResolveColorTexture(FramebufferTarget::Read, srcTex, srcW, srcH, srcFmt) || !srcTex) {
            return;
        }
        const Uint32 w = static_cast<Uint32>(width);
        const Uint32 h = static_cast<Uint32>(height);
        // Render targets are stored in GL texel order (row 0 = GL window bottom; see
        // RemapClipSpace), so glReadPixels' bottom-up output is texel rows [y, y+h) in
        // natural order — no flip. The stored bytes are returned as-is (no sRGB
        // encode/decode happens on readback, even for sRGB framebuffers).
        (void)srcH;
        ReadTextureToCPU(srcTex, static_cast<Uint32>(x < 0 ? 0 : x), static_cast<Uint32>(y < 0 ? 0 : y),
                         w, h, srcFmt, format, /*flipY=*/false, pixels);
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
        // Submit pending draws so the readback reflects everything rendered so far.
        if (m_frameActive) {
            FlushFrame();
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
        ReadTextureToCPU(wt->texture, 0, 0, w, h, wt->format, format, /*flipY=*/false, pixels);
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
