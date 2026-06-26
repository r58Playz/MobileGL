// MobileGL - MobileGL/MG_Backend/DirectWebGPU/BackendObject_DirectWebGPU.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectWebGPU.h"

namespace MobileGL::MG_Backend::DirectWebGPU {
    BackendObject_DirectWebGPU::~BackendObject_DirectWebGPU() = default;

    void BackendObject_DirectWebGPU::Initialize() {
        // M0 stub: no device/surface yet. Just populate identity strings so that
        // LogBackendInfo() and GetProcAddress-driven queries have something sane.
        m_rendererInfo.RendererName = "MobileGL (DirectWebGPU stub)";
        m_rendererInfo.BackendName = "WebGPU";
        m_rendererInfo.RendererGLInfo.TargetGLVersion.Major = 3;
        m_rendererInfo.RendererGLInfo.TargetGLVersion.Minor = 3;
        m_rendererInfo.RendererGLInfo.TargetGLSLVersion.Major = 3;
        m_rendererInfo.RendererGLInfo.TargetGLSLVersion.Minor = 30;
        m_rendererInfo.RendererGLInfo.IsCompatibilityProfile = false;
        MGLOG_W("DirectWebGPU backend is a stub (M0); no rendering is performed yet");
    }

    Bool BackendObject_DirectWebGPU::InitCapabilities() {
        return true;
    }

    Bool BackendObject_DirectWebGPU::InitWindowSurface() {
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
