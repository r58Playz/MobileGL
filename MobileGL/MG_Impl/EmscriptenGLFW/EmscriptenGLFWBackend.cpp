// MobileGL - MobileGL/MG_Impl/EmscriptenGLFW/EmscriptenGLFWBackend.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// GL-translation-backend adapter consumed by the patched pongasoft emscripten-glfw
// (ikvm-wasm-build/tools/native-deps/emscripten-glfw.patch). That patch routes GLFW's
// WebGL context lifecycle and GL proc-address resolution through a compile-time-selected
// symbol prefix; the non-Krypton branch expects these `mobilegl_*` C entry points:
//
//   EMSCRIPTEN_WEBGL_CONTEXT_HANDLE mobilegl_webgl_create_context(target, attrs)
//   EMSCRIPTEN_RESULT               mobilegl_webgl_make_context_current(ctx)
//   EMSCRIPTEN_RESULT               mobilegl_webgl_destroy_context(ctx)
//   void*                           mobilegl_get_proc_address(name)
//   void                            mobilegl_present()          // added to the seam (WebGPU doesn't auto-present)
//   void                            mobilegl_resize(w, h)       // added to the seam (WebGPU surface reconfigure)
//   void                            mobilegl_proc_init()        // no-op; MobileGL auto-inits via a constructor
//
// Rather than the legacy WebGL path (emscripten_webgl_create_context), these drive MobileGL's
// own EGL 1.5 front-end, whose Emscripten backend renders through WebGPU (DirectWebGPU). The
// returned EMSCRIPTEN_WEBGL_CONTEXT_HANDLE is an opaque positive int mapped to the EGL
// display/surface/context triple.

#if defined(__EMSCRIPTEN__) && defined(MOBILEGL_EMSCRIPTEN_GLFW)

#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <EGL/egl.h>

#include "MG_Impl/EGLImpl/EGLImpl.h"
#include "MG_Impl/GetProcAddress.h"

#ifdef MOBILEGL_EMSCRIPTEN_GLFW_FPE
// SimpleFPEWrapper front layer: emulates the fixed-function pipeline (Minecraft 1.16 and
// lower) on top of MobileGL's GL 3.3 core. Its downstream is wired to MobileGL below.
// SFPEW's front entry point `eglGetProcAddress` and its GL symbols are renamed to sfpew_*
// (objcopy --redefine-syms in the build) so they don't collide with MobileGL's own gl*/egl*.
extern "C" __eglMustCastToProperFunctionPointerType sfpew_eglGetProcAddress(const char* name);
extern "C" void sfpew_emscripten_init(__eglMustCastToProperFunctionPointerType (*downstreamProc)(const char*));
#endif

// MobileGL auto-inits via a static constructor (MobileGL/Init.cpp), but a TU containing only a
// static constructor can be dropped when libMobileGL.a is linked as an archive, so call it
// explicitly (as the standalone harness does) before touching EGL.
namespace MobileGL { void Initialize(); }

namespace {
    namespace EGLImpl = MobileGL::MG_Impl::EGLImpl;

    struct GLContextEntry {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLSurface surface = EGL_NO_SURFACE;
        EGLContext context = EGL_NO_CONTEXT;
        int width = 0;
        int height = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<int, GLContextEntry> g_contexts;
    int g_nextHandle = 1;
    int g_currentHandle = 0;

    // The single lazily-initialized EGL display (EGL_DEFAULT_DISPLAY).
    EGLDisplay g_display = EGL_NO_DISPLAY;
    bool g_displayInitialized = false;

    // Downstream proc-address MobileGL exposes to the fixed-function front layer, cast to the
    // EGL proc-address signature. MobileGL::MG_Impl::GetProcAddress resolves both gl* and egl*.
    __eglMustCastToProperFunctionPointerType MobileGLProc(const char* name) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(MobileGL::MG_Impl::GetProcAddress(name));
    }

    bool EnsureDisplay() {
        if (g_displayInitialized) {
            return g_display != EGL_NO_DISPLAY;
        }
        g_displayInitialized = true;
        MobileGL::Initialize();
        g_display = EGLImpl::GetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_display == EGL_NO_DISPLAY) {
            std::fprintf(stderr, "[mobilegl] eglGetDisplay(EGL_DEFAULT_DISPLAY) failed\n");
            return false;
        }
        if (!EGLImpl::Initialize(g_display, nullptr, nullptr)) {
            std::fprintf(stderr, "[mobilegl] eglInitialize failed\n");
            g_display = EGL_NO_DISPLAY;
            return false;
        }
        return true;
    }

    GLContextEntry* Lookup(int handle) {
        auto it = g_contexts.find(handle);
        return it == g_contexts.end() ? nullptr : &it->second;
    }
} // namespace

extern "C" {

EMSCRIPTEN_WEBGL_CONTEXT_HANDLE mobilegl_webgl_create_context(const char* target,
                                                              const EmscriptenWebGLContextAttributes* attributes) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureDisplay()) {
        return 0;
    }

    EGLImpl::BindAPI(EGL_OPENGL_API);

    const int depthBits = (attributes && attributes->depth) ? 24 : 0;
    const int stencilBits = (attributes && attributes->stencil) ? 8 : 0;
    const int alphaBits = (attributes && attributes->alpha) ? 8 : 0;

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      alphaBits,
        EGL_DEPTH_SIZE,      depthBits,
        EGL_STENCIL_SIZE,    stencilBits,
        EGL_NONE,
    };

    EGLConfig config = nullptr;
    EGLint numConfig = 0;
    if (!EGLImpl::ChooseConfig(g_display, configAttribs, &config, 1, &numConfig) || numConfig <= 0) {
        std::fprintf(stderr, "[mobilegl] eglChooseConfig failed\n");
        return 0;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    EGLContext context = EGLImpl::CreateContext(g_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[mobilegl] eglCreateContext failed\n");
        return 0;
    }

    int width = 0;
    int height = 0;
    emscripten_get_canvas_element_size(target && target[0] ? target : "#canvas", &width, &height);
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;

    const EGLint surfaceAttribs[] = {
        EGL_WIDTH,  width,
        EGL_HEIGHT, height,
        EGL_NONE,
    };
    // The native-window handle only has to be non-null; DirectWebGPU takes the canvas selector
    // internally (currently "#canvas", matching emscripten-glfw's patched default selector).
    EGLSurface surface = EGLImpl::CreateWindowSurface(g_display, config,
                                                      reinterpret_cast<NativeWindowType>(1), surfaceAttribs);
    if (surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[mobilegl] eglCreateWindowSurface failed\n");
        EGLImpl::DestroyContext(g_display, context);
        return 0;
    }

    const int handle = g_nextHandle++;
    g_contexts[handle] = GLContextEntry{g_display, surface, context, width, height};
    return static_cast<EMSCRIPTEN_WEBGL_CONTEXT_HANDLE>(handle);
}

EMSCRIPTEN_RESULT mobilegl_webgl_make_context_current(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (ctx == 0) {
        EGLImpl::MakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_currentHandle = 0;
        return EMSCRIPTEN_RESULT_SUCCESS;
    }

    GLContextEntry* entry = Lookup(static_cast<int>(ctx));
    if (!entry) {
        return EMSCRIPTEN_RESULT_INVALID_TARGET;
    }
    if (!EGLImpl::MakeCurrent(entry->display, entry->surface, entry->surface, entry->context)) {
        std::fprintf(stderr, "[mobilegl] eglMakeCurrent failed\n");
        return EMSCRIPTEN_RESULT_FAILED;
    }
    g_currentHandle = static_cast<int>(ctx);

#ifdef MOBILEGL_EMSCRIPTEN_GLFW_FPE
    // Wire the fixed-function front layer to MobileGL once a context is current (so GL queries
    // used while acquiring downstream functions have a live context). Idempotent.
    static bool fpeInitialized = false;
    if (!fpeInitialized) {
        fpeInitialized = true;
        sfpew_emscripten_init(&MobileGLProc);
    }
#endif
    return EMSCRIPTEN_RESULT_SUCCESS;
}

EMSCRIPTEN_RESULT mobilegl_webgl_destroy_context(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx) {
    std::lock_guard<std::mutex> lock(g_mutex);

    GLContextEntry* entry = Lookup(static_cast<int>(ctx));
    if (!entry) {
        return EMSCRIPTEN_RESULT_INVALID_TARGET;
    }
    if (g_currentHandle == static_cast<int>(ctx)) {
        EGLImpl::MakeCurrent(entry->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_currentHandle = 0;
    }
    EGLImpl::DestroySurface(entry->display, entry->surface);
    EGLImpl::DestroyContext(entry->display, entry->context);
    g_contexts.erase(static_cast<int>(ctx));
    return EMSCRIPTEN_RESULT_SUCCESS;
}

void* mobilegl_get_proc_address(const char* name) {
#ifdef MOBILEGL_EMSCRIPTEN_GLFW_FPE
    return reinterpret_cast<void*>(sfpew_eglGetProcAddress(name));
#else
    return MobileGL::MG_Impl::GetProcAddress(name);
#endif
}

// Present the current surface. emscripten-glfw's Context::swapBuffers() calls this once per
// presented frame for the mobilegl backend (in place of the WebGL glFlush()/newRenderingFrameStarted()).
void mobilegl_present() {
    std::lock_guard<std::mutex> lock(g_mutex);
    GLContextEntry* entry = Lookup(g_currentHandle);
    if (!entry) {
        return;
    }
    EGLImpl::SwapBuffers(entry->display, entry->surface);
}

// Reconfigure the WebGPU surface when the GLFW window/canvas is resized.
void mobilegl_resize(int width, int height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (width <= 0 || height <= 0) {
        return;
    }
    GLContextEntry* entry = Lookup(g_currentHandle);
    if (!entry) {
        return;
    }
    if (entry->width == width && entry->height == height) {
        return;
    }
    entry->width = width;
    entry->height = height;
    EGLImpl::ResizePlatformWindowSurface(entry->display, entry->surface, width, height);
}

// MobileGL auto-initializes via a static constructor (MobileGL/Init.cpp AutoInit()), so this is a
// no-op. It exists to mirror the client's existing backend-init call site (was krypton_proc_init()).
void mobilegl_proc_init() {}

} // extern "C"

#endif // __EMSCRIPTEN__ && MOBILEGL_EMSCRIPTEN_GLFW
