// MobileGL - native Dawn DirectWebGPU platform bootstrap.
#if !defined(__EMSCRIPTEN__) && defined(MOBILEGL_NATIVE_WEBGPU)

#include "WebGPUPlatform.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace MobileGL::MG_Backend::DirectWebGPU::Platform {
    namespace {
        struct AdapterRequest {
            std::atomic_bool Done = false;
            WGPURequestAdapterStatus Status = WGPURequestAdapterStatus_Error;
            WGPUAdapter Adapter = nullptr;
            String Message;
        };

        void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                       void* userdata1, void*) {
            auto* request = static_cast<AdapterRequest*>(userdata1);
            request->Status = status;
            request->Adapter = adapter;
            request->Message.assign(message.data ? message.data : "", message.length);
            request->Done = true;
        }

        struct DeviceRequest {
            std::atomic_bool Done = false;
            WGPURequestDeviceStatus Status = WGPURequestDeviceStatus_Error;
            WGPUDevice Device = nullptr;
            String Message;
        };

        void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                      void* userdata1, void*) {
            auto* request = static_cast<DeviceRequest*>(userdata1);
            request->Status = status;
            request->Device = device;
            request->Message.assign(message.data ? message.data : "", message.length);
            request->Done = true;
        }

        void OnUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message,
                               void*, void*) {
            MGLOG_E("DirectWebGPU/Dawn validation error (%d): %.*s", static_cast<int>(type),
                    static_cast<int>(message.length), message.data ? message.data : "");
        }

        void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message,
                          void*, void*) {
            MGLOG_E("DirectWebGPU/Dawn device lost (%d): %.*s", static_cast<int>(reason),
                    static_cast<int>(message.length), message.data ? message.data : "");
        }
    } // namespace

    void WaitForCallback(WGPUInstance instance, const std::atomic_bool& completed) {
        while (!completed.load(std::memory_order_acquire)) {
            wgpuInstanceProcessEvents(instance);
            std::this_thread::yield();
        }
    }

    void SignalCallback() {}

    Bool Initialize(const InitInfo& info, Handles& handles) {
        handles.Instance = wgpuCreateInstance(nullptr);
        if (!handles.Instance) {
            MGLOG_E("DirectWebGPU/Dawn: wgpuCreateInstance failed");
            return false;
        }

        if (!info.Headless) {
            if (!info.NativeDisplay || !info.NativeWindow) {
                MGLOG_E("DirectWebGPU/Dawn: X11 display/window is missing");
                return false;
            }
            WGPUSurfaceSourceXlibWindow source{};
            source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
            source.display = info.NativeDisplay;
            source.window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.NativeWindow));
            WGPUSurfaceDescriptor descriptor{};
            descriptor.nextInChain = &source.chain;
            handles.Surface = wgpuInstanceCreateSurface(handles.Instance, &descriptor);
            if (!handles.Surface) {
                MGLOG_E("DirectWebGPU/Dawn: failed to create Xlib surface");
                return false;
            }
        }

        AdapterRequest adapterRequest;
        WGPURequestAdapterOptions adapterOptions{};
        adapterOptions.backendType = WGPUBackendType_Vulkan;
        adapterOptions.compatibleSurface = handles.Surface;
        WGPURequestAdapterCallbackInfo adapterCallback{};
        adapterCallback.mode = WGPUCallbackMode_AllowProcessEvents;
        adapterCallback.callback = &OnAdapter;
        adapterCallback.userdata1 = &adapterRequest;
        wgpuInstanceRequestAdapter(handles.Instance, &adapterOptions, adapterCallback);
        WaitForCallback(handles.Instance, adapterRequest.Done);
        if (adapterRequest.Status != WGPURequestAdapterStatus_Success || !adapterRequest.Adapter) {
            MGLOG_E("DirectWebGPU/Dawn: adapter request failed: %s", adapterRequest.Message.c_str());
            return false;
        }

        DeviceRequest deviceRequest;
        WGPUDeviceDescriptor deviceDescriptor{};
        WGPUSupportedFeatures supported{};
        wgpuAdapterGetFeatures(adapterRequest.Adapter, &supported);
        const WGPUFeatureName optionalFeatures[] = {
            WGPUFeatureName_RG11B10UfloatRenderable,
            WGPUFeatureName_Float32Filterable,
        };
        std::vector<WGPUFeatureName> requiredFeatures;
        for (WGPUFeatureName wanted : optionalFeatures) {
            if (std::find(supported.features, supported.features + supported.featureCount, wanted) !=
                supported.features + supported.featureCount) {
                requiredFeatures.push_back(wanted);
            }
        }
        wgpuSupportedFeaturesFreeMembers(supported);
        deviceDescriptor.requiredFeatureCount = requiredFeatures.size();
        deviceDescriptor.requiredFeatures = requiredFeatures.data();
        deviceDescriptor.uncapturedErrorCallbackInfo.callback = &OnUncapturedError;
        deviceDescriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        deviceDescriptor.deviceLostCallbackInfo.callback = &OnDeviceLost;
        WGPURequestDeviceCallbackInfo deviceCallback{};
        deviceCallback.mode = WGPUCallbackMode_AllowProcessEvents;
        deviceCallback.callback = &OnDevice;
        deviceCallback.userdata1 = &deviceRequest;
        wgpuAdapterRequestDevice(adapterRequest.Adapter, &deviceDescriptor, deviceCallback);
        WaitForCallback(handles.Instance, deviceRequest.Done);
        if (deviceRequest.Status != WGPURequestDeviceStatus_Success || !deviceRequest.Device) {
            wgpuAdapterRelease(adapterRequest.Adapter);
            MGLOG_E("DirectWebGPU/Dawn: device request failed: %s", deviceRequest.Message.c_str());
            return false;
        }

        handles.Device = deviceRequest.Device;
        handles.OwnsDevice = true;
        handles.Queue = wgpuDeviceGetQueue(handles.Device);
        handles.SurfaceFormat = WGPUTextureFormat_BGRA8Unorm;
        if (handles.Surface) {
            WGPUSurfaceCapabilities capabilities{};
            if (wgpuSurfaceGetCapabilities(handles.Surface, adapterRequest.Adapter, &capabilities) ==
                    WGPUStatus_Success && capabilities.formatCount > 0) {
                handles.SurfaceFormat = capabilities.formats[0];
                for (size_t i = 0; i < capabilities.formatCount; ++i) {
                    if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                        capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                        handles.SurfaceFormat = capabilities.formats[i];
                        break;
                    }
                }
            }
            wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        }
        wgpuAdapterRelease(adapterRequest.Adapter);
        handles.Width = std::max<Uint32>(info.Width, 1);
        handles.Height = std::max<Uint32>(info.Height, 1);
        MGLOG_I("DirectWebGPU/Dawn initialized: Vulkan adapter, %ux%u, headless=%d",
                info.Width, info.Height, info.Headless ? 1 : 0);
        return handles.Queue != nullptr;
    }

    void Present(WGPUSurface surface) {
        if (surface) wgpuSurfacePresent(surface);
    }

    void Shutdown(Handles& handles) {
        if (handles.Surface) wgpuSurfaceRelease(handles.Surface);
        if (handles.Queue) wgpuQueueRelease(handles.Queue);
        if (handles.OwnsDevice && handles.Device) wgpuDeviceRelease(handles.Device);
        if (handles.Instance) wgpuInstanceRelease(handles.Instance);
        handles = {};
    }
} // namespace MobileGL::MG_Backend::DirectWebGPU::Platform
#endif
