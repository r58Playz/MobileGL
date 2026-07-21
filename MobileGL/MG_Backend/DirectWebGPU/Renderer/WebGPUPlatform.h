// MobileGL - platform ownership/bootstrap for DirectWebGPU.
#pragma once

#include <Includes.h>
#include "../WgpuApi.h"
#include <atomic>

namespace MobileGL::MG_Backend::DirectWebGPU::Platform {
    struct InitInfo {
        String CanvasSelector;
        void* NativeDisplay = nullptr;
        void* NativeWindow = nullptr;
        Uint32 Width = 1;
        Uint32 Height = 1;
        Bool Headless = false;
    };

    struct Handles {
        WGPUInstance Instance = nullptr;
        WGPUDevice Device = nullptr;
        WGPUQueue Queue = nullptr;
        WGPUSurface Surface = nullptr;
        WGPUTextureFormat SurfaceFormat = WGPUTextureFormat_BGRA8Unorm;
        Uint32 Width = 1;
        Uint32 Height = 1;
        Bool OwnsDevice = false;
    };

    Bool Initialize(const InitInfo& info, Handles& handles);
    void WaitForCallback(WGPUInstance instance, const std::atomic_bool& completed);
    void SignalCallback();
    void Present(WGPUSurface surface);
    void Shutdown(Handles& handles);
} // namespace MobileGL::MG_Backend::DirectWebGPU::Platform
