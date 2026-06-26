// MobileGL - MobileGL/MG_Backend/DirectWebGPU/BackendObject_DirectWebGPU.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BackendObject.h"

namespace MobileGL::MG_Backend::DirectWebGPU {
    // M0 stub: implements the BackendObject interface so MG_Backend::Init links and
    // the wasm library builds. No actual WebGPU rendering yet (added in M2).
    class BackendObject_DirectWebGPU : public BackendObject {
    public:
        ~BackendObject_DirectWebGPU() override;

        void Initialize() override;
        Bool InitCapabilities() override;
        Bool InitWindowSurface() override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;

    private:
        RendererInfo m_rendererInfo;
        GlobalBackendFunctionsTable m_backendFunctions{};
        DynamicBackendParameters m_dynamicParameters{};
    };
} // namespace MobileGL::MG_Backend::DirectWebGPU
