// MobileGL - Emscripten DirectWebGPU platform bootstrap.
#if defined(__EMSCRIPTEN__)

#include "WebGPUPlatform.h"

#include <emscripten/html5.h>

extern "C" int mobilegl_preferred_canvas_format();
extern "C" int mobilegl_has_webgpu_device();
extern "C" void mobilegl_acquire_webgpu_device();
extern "C" void mobilegl_jspi_wait();
extern "C" void mobilegl_jspi_signal();

namespace MobileGL::MG_Backend::DirectWebGPU::Platform {
    Bool Initialize(const InitInfo& info, Handles& handles) {
        handles.Instance = wgpuCreateInstance(nullptr);
        if (!handles.Instance) return false;

        if (!mobilegl_has_webgpu_device()) mobilegl_acquire_webgpu_device();
        handles.Device = emscripten_webgpu_get_device();
        if (!handles.Device) return false;
        handles.Queue = wgpuDeviceGetQueue(handles.Device);
        handles.SurfaceFormat = mobilegl_preferred_canvas_format() == 1
                                    ? WGPUTextureFormat_RGBA8Unorm
                                    : WGPUTextureFormat_BGRA8Unorm;

        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source{};
        source.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        source.selector = Wgpu::View(info.CanvasSelector.c_str());
        WGPUSurfaceDescriptor descriptor{};
        descriptor.nextInChain = &source.chain;
        handles.Surface = wgpuInstanceCreateSurface(handles.Instance, &descriptor);
        int width = 0;
        int height = 0;
        emscripten_get_canvas_element_size(info.CanvasSelector.c_str(), &width, &height);
        handles.Width = width > 0 ? static_cast<Uint32>(width) : info.Width;
        handles.Height = height > 0 ? static_cast<Uint32>(height) : info.Height;
        return handles.Surface != nullptr;
    }

    void WaitForCallback(WGPUInstance, const std::atomic_bool&) { mobilegl_jspi_wait(); }
    void SignalCallback() { mobilegl_jspi_signal(); }
    void Present(WGPUSurface) {}

    void Shutdown(Handles& handles) {
        if (handles.Surface) wgpuSurfaceRelease(handles.Surface);
        if (handles.Queue) wgpuQueueRelease(handles.Queue);
        // The browser owns the preinitialized device.
        if (handles.Instance) wgpuInstanceRelease(handles.Instance);
        handles = {};
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU::Platform
#endif
