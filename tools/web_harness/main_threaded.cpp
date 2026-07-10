// MobileGL threaded web harness — validates the FULL emscripten pipeline the ikvmcraft
// client uses: WASM pthreads + a transferred OffscreenCanvas owned by the render worker +
// the DirectWebGPU backend, driven through the real mobilegl_* GL-backend adapter.
//
// Faithful to ikvmcraft:
//   * built with -pthread and PROXY_TO_PTHREAD so main() (and all GL work) runs on a worker,
//   * the canvas is transferControlToOffscreen'd to that worker via the SAME monkeypatch the
//     ikvmcraft Makefile injects (Module.transferredCanvasNames — see shell_threaded.html),
//   * the WebGPU device is acquired ON THE WORKER (navigator.gpu) and stashed in the worker's
//     Module.preinitializedWebGPUDevice, exactly as the de-risk concluded (no cross-thread
//     device transfer),
//   * rendering goes App -> mobilegl_webgl_* adapter -> MobileGL EGL -> DirectWebGPU -> present.
//
// Success = a green canvas + a "[harness]" log line reporting the live GL_VERSION/RENDERER
// strings that came back through mobilegl_get_proc_address.

#include <cstdio>
#include <cstdint>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <EGL/egl.h>
#include <GL/glcorearb.h>

// The adapter contract exported by libMobileGL.a (built with -DMOBILEGL_EMSCRIPTEN_GLFW=ON).
extern "C" {
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE mobilegl_webgl_create_context(const char* target,
                                                              const EmscriptenWebGLContextAttributes* attributes);
EMSCRIPTEN_RESULT mobilegl_webgl_make_context_current(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx);
void* mobilegl_get_proc_address(const char* name);
void mobilegl_present();
}

using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void (*)(GLbitfield);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glGetString = const GLubyte* (*)(GLenum);
using PFN_glGetError = GLenum (*)();

extern "C" EMSCRIPTEN_KEEPALIVE void run_render() {
    printf("[harness] run_render on worker; acquiring GL context via mobilegl adapter\n");

    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.depth = 1;
    attrs.stencil = 1;
    attrs.alpha = 0;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = mobilegl_webgl_create_context("#canvas", &attrs);
    if (ctx == 0) {
        printf("[harness] FAIL: mobilegl_webgl_create_context returned 0\n");
        return;
    }
    if (mobilegl_webgl_make_context_current(ctx) != EMSCRIPTEN_RESULT_SUCCESS) {
        printf("[harness] FAIL: mobilegl_webgl_make_context_current failed\n");
        return;
    }

    auto glClearColor_ = reinterpret_cast<PFN_glClearColor>(mobilegl_get_proc_address("glClearColor"));
    auto glClear_ = reinterpret_cast<PFN_glClear>(mobilegl_get_proc_address("glClear"));
    auto glViewport_ = reinterpret_cast<PFN_glViewport>(mobilegl_get_proc_address("glViewport"));
    auto glGetString_ = reinterpret_cast<PFN_glGetString>(mobilegl_get_proc_address("glGetString"));
    auto glGetError_ = reinterpret_cast<PFN_glGetError>(mobilegl_get_proc_address("glGetError"));

    if (!glClearColor_ || !glClear_ || !glGetString_) {
        printf("[harness] FAIL: mobilegl_get_proc_address returned null (clearColor=%p clear=%p getString=%p)\n",
               (void*)glClearColor_, (void*)glClear_, (void*)glGetString_);
        return;
    }

    const GLubyte* ver = glGetString_(GL_VERSION);
    const GLubyte* rend = glGetString_(GL_RENDERER);
    const GLubyte* vend = glGetString_(GL_VENDOR);
    printf("[harness] GL_VERSION=%s | GL_RENDERER=%s | GL_VENDOR=%s\n",
           ver ? (const char*)ver : "(null)", rend ? (const char*)rend : "(null)",
           vend ? (const char*)vend : "(null)");

    if (glViewport_) glViewport_(0, 0, 512, 512);
    glClearColor_(0.1f, 0.9f, 0.2f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glGetError_) {
        GLenum e = glGetError_();
        printf("[harness] glGetError after clear = 0x%x\n", (unsigned)e);
    }
    mobilegl_present();

    printf("[harness] SUCCESS: presented a frame through mobilegl adapter on the render worker\n");
    // Signal completion to the page for the automated check.
    EM_ASM({ if (globalThis.postMessage) {} Module['__harnessDone'] = true; });
}

int main() {
    printf("[harness] main() running (PROXY_TO_PTHREAD worker=%d)\n", (int)emscripten_is_main_runtime_thread());
    // Acquire the WebGPU device ON THIS WORKER, then drive the render. Async is fine: with
    // EXIT_RUNTIME=0 the worker event loop stays alive to run the .then, which calls back into
    // wasm (_run_render) — no JSPI needed for a clear+present.
    EM_ASM({
        (async function() {
            try {
                if (!navigator.gpu) { console.error('[harness] navigator.gpu missing on worker'); return; }
                var adapter = await navigator.gpu.requestAdapter();
                var device = await adapter.requestDevice();
                Module['preinitializedWebGPUDevice'] = device;
                console.log('[harness] worker acquired WebGPU device; calling _run_render');
                Module['_run_render']();
            } catch (e) {
                console.error('[harness] device/render failed on worker: ' + (e && e.stack || e));
            }
        })();
    });
    return 0;
}
