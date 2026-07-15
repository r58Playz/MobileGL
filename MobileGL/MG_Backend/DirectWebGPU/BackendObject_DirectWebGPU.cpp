// MobileGL - MobileGL/MG_Backend/DirectWebGPU/BackendObject_DirectWebGPU.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectWebGPU.h"
#include "DirectWebGPU.h"
#include "Renderer/WebGPURenderer.h"

namespace MobileGL::MG_Backend::DirectWebGPU {
    BackendObject_DirectWebGPU::~BackendObject_DirectWebGPU() {
        pWebGPURenderer.reset();
    }

    void BackendObject_DirectWebGPU::Initialize() {
        m_rendererInfo.RendererName = "MobileGL (DirectWebGPU)";
        m_rendererInfo.BackendName = "WebGPU";
        m_rendererInfo.RendererGLInfo.TargetGLVersion.Major = 3;
        m_rendererInfo.RendererGLInfo.TargetGLVersion.Minor = 3;
        m_rendererInfo.RendererGLInfo.TargetGLSLVersion.Major = 3;
        m_rendererInfo.RendererGLInfo.TargetGLSLVersion.Minor = 30;
        m_rendererInfo.RendererGLInfo.IsCompatibilityProfile = false;

        // Wire the function table. M2a implements clear + present; the remaining
        // GL entry points arrive with the draw path in M2b.
        m_backendFunctions = {};
        m_backendFunctions.Present = &DirectWebGPU::Present;
        m_backendFunctions.GL.Clear = &DirectWebGPU::Clear;
        m_backendFunctions.GL.DrawArrays = &DirectWebGPU::DrawArrays;
        m_backendFunctions.GL.DrawElements = &DirectWebGPU::DrawElements;
        m_backendFunctions.GL.DrawElementsBaseVertex = &DirectWebGPU::DrawElementsBaseVertex;
        m_backendFunctions.GL.DrawRangeElements = &DirectWebGPU::DrawRangeElements;
        m_backendFunctions.GL.DrawRangeElementsBaseVertex = &DirectWebGPU::DrawRangeElementsBaseVertex;
        m_backendFunctions.GL.DrawArraysInstanced = &DirectWebGPU::DrawArraysInstanced;
        m_backendFunctions.GL.DrawArraysInstancedBaseInstance = &DirectWebGPU::DrawArraysInstancedBaseInstance;
        m_backendFunctions.GL.DrawElementsInstanced = &DirectWebGPU::DrawElementsInstanced;
        m_backendFunctions.GL.DrawElementsInstancedBaseVertex = &DirectWebGPU::DrawElementsInstancedBaseVertex;
        m_backendFunctions.GL.DrawElementsInstancedBaseInstance =
            &DirectWebGPU::DrawElementsInstancedBaseInstance;
        m_backendFunctions.GL.DrawElementsInstancedBaseVertexBaseInstance =
            &DirectWebGPU::DrawElementsInstancedBaseVertexBaseInstance;
        m_backendFunctions.GL.MultiDrawElementsBaseVertex = &DirectWebGPU::MultiDrawElementsBaseVertex;
        m_backendFunctions.GL.MultiDrawElements = &DirectWebGPU::MultiDrawElements;
        m_backendFunctions.GL.BlitFramebuffer = &DirectWebGPU::BlitFramebuffer;
        m_backendFunctions.GL.ReadPixels = &DirectWebGPU::ReadPixels;
        m_backendFunctions.GL.GetTexImage = &DirectWebGPU::GetTexImage;
        m_backendFunctions.GL.GetTextureImage = &DirectWebGPU::GetTextureImage;
        // Safe no-ops (frontend calls these unguarded; see DirectWebGPU.cpp).
        m_backendFunctions.GL.GenerateMipmap = &DirectWebGPU::GenerateMipmap;
        m_backendFunctions.GL.CopyTexSubImage2D = &DirectWebGPU::CopyTexSubImage2D;
        m_backendFunctions.GL.CopyTexImage2D = &DirectWebGPU::CopyTexImage2D;
        m_backendFunctions.GL.Finish = &DirectWebGPU::Finish;
        m_backendFunctions.GL.Flush = &DirectWebGPU::Flush;
    }

    Bool BackendObject_DirectWebGPU::InitCapabilities() {
        return true;
    }

    Bool BackendObject_DirectWebGPU::InitWindowSurface() {
        // TODO(M3): thread the canvas selector through WindowHandle; default for now.
        pWebGPURenderer = MakeUnique<WebGPURenderer>();
        if (!pWebGPURenderer->Initialize("#canvas")) {
            MGLOG_E("DirectWebGPU: failed to initialize WebGPU renderer");
            pWebGPURenderer.reset();
            return false;
        }
        return true;
    }

    const RendererInfo& BackendObject_DirectWebGPU::GetRendererInfo() const {
        return m_rendererInfo;
    }

    String BackendObject_DirectWebGPU::GetBackendAPIVersionString() const {
        return "WebGPU (emdawnwebgpu)";
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectWebGPU::GetBackendFunctions() const {
        return m_backendFunctions;
    }

    const DynamicBackendParameters& BackendObject_DirectWebGPU::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    BackendType BackendObject_DirectWebGPU::GetBackendType() const {
        return BackendType::DirectWebGPU;
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU
