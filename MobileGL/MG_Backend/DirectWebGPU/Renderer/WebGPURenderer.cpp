// MobileGL - MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WebGPURenderer.h"
#include <MG_State/GLState/Core.h>
#include <emscripten/html5.h>

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
} // namespace MobileGL::MG_Backend::DirectWebGPU
