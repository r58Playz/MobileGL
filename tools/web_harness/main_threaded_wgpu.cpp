// Minimal emdawnwebgpu harness that reproduces the ikvmcraft render-worker plumbing WITHOUT
// MobileGL/tint (so it links -pthread cleanly): emscripten pthreads + PROXY_TO_PTHREAD + a
// transferred OffscreenCanvas ('.canvas', the same name ikvmcraft's monkeypatch transfers) +
// a WebGPU device acquired ON THE WORKER + a WebGPU surface created from the '#canvas' selector
// (the selector MobileGL/emscripten-glfw actually use) + clear-to-green + implicit present.
//
// It uses the exact emdawnwebgpu webgpu.cpp + --js-library files MobileGL's DirectWebGPU backend
// uses, and mirrors WebGPURenderer::Initialize/AcquireSurfaceView. If this shows green, the
// emscripten-pthread + OffscreenCanvas + emdawnwebgpu-on-worker path (the only pieces unproven
// in a threaded context) is validated, and it also directly tests the '.canvas'-transfer vs
// '#canvas'-lookup selector question.

#include <cstdio>
#include <cstring>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <webgpu/webgpu.h>

extern "C" WGPUDevice emscripten_webgpu_get_device(void);

static WGPUStringView sv(const char* s) {
    WGPUStringView v;
    v.data = s;
    v.length = std::strlen(s);
    return v;
}

extern "C" EMSCRIPTEN_KEEPALIVE void run_render() {
    printf("[wgpu-harness] run_render on worker (is_main_runtime_thread=%d)\n",
           (int)emscripten_is_main_runtime_thread());

    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) { printf("[wgpu-harness] FAIL: wgpuCreateInstance\n"); return; }

    WGPUDevice device = emscripten_webgpu_get_device();
    if (!device) { printf("[wgpu-harness] FAIL: emscripten_webgpu_get_device (worker Module.preinitializedWebGPUDevice not set?)\n"); return; }
    WGPUQueue queue = wgpuDeviceGetQueue(device);
    printf("[wgpu-harness] got device+queue on worker\n");

    const char* selector = "#canvas"; // what MobileGL/emscripten-glfw use; canvas transferred as '.canvas'
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSrc = {};
    canvasSrc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasSrc.selector = sv(selector);
    WGPUSurfaceDescriptor surfaceDesc = {};
    surfaceDesc.nextInChain = &canvasSrc.chain;
    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfaceDesc);
    if (!surface) { printf("[wgpu-harness] FAIL: wgpuInstanceCreateSurface('%s')\n", selector); return; }
    printf("[wgpu-harness] created surface from selector '%s' on worker\n", selector);

    int w = 0, h = 0;
    emscripten_get_canvas_element_size(selector, &w, &h);
    printf("[wgpu-harness] emscripten_get_canvas_element_size('%s') -> %dx%d\n", selector, w, h);
    if (w <= 0) w = 512;
    if (h <= 0) h = 512;

    WGPUSurfaceConfiguration cfg = {};
    cfg.device = device;
    cfg.format = WGPUTextureFormat_BGRA8Unorm;
    cfg.usage = WGPUTextureUsage_RenderAttachment;
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
    cfg.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &cfg);

    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(surface, &st);
    printf("[wgpu-harness] wgpuSurfaceGetCurrentTexture status=%d (SuccessOptimal=%d)\n",
           (int)st.status, (int)WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal);
    if (!st.texture) { printf("[wgpu-harness] FAIL: no surface texture\n"); return; }

    WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPURenderPassColorAttachment ca = {};
    ca.view = view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = WGPUColor{0.1, 0.9, 0.2, 1.0};
    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);

    printf("[wgpu-harness] SUCCESS: submitted clear-to-green on worker; browser presents implicitly\n");
    EM_ASM({ Module['__harnessDone'] = true; });
}

int main() {
    printf("[wgpu-harness] main() (is_main_runtime_thread=%d)\n", (int)emscripten_is_main_runtime_thread());
    // Acquire the WebGPU device ON THIS WORKER, then call back into wasm to render. No JSPI
    // needed: EXIT_RUNTIME=0 keeps the worker event loop alive to run the async .then.
    EM_ASM({
        (async function() {
            try {
                if (!navigator.gpu) { console.error('[wgpu-harness] navigator.gpu missing on worker'); return; }
                var adapter = await navigator.gpu.requestAdapter();
                var device = await adapter.requestDevice();
                Module['preinitializedWebGPUDevice'] = device;
                console.log('[wgpu-harness] worker acquired device; calling _run_render');
                Module['_run_render']();
            } catch (e) {
                console.error('[wgpu-harness] worker failure: ' + (e && e.stack || e));
            }
        })();
    });
    return 0;
}
