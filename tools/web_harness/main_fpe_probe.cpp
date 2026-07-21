// Part-A validation probe: links libMobileGL.a (MOBILEGL_EMSCRIPTEN_GLFW[_FPE]=ON) together with
// the symbol-renamed SimpleFPEWrapper archive and drives the mobilegl_* adapter on the main thread.
// It proves (1) the two archives LINK with no duplicate gl*/egl* symbols, (2) mobilegl_get_proc_address
// routes through SFPEW (a fixed-function-only name like "glBegin" resolves to SFPEW's emulation),
// and (3) SFPEW falls through to MobileGL for core names ("glClear"/"glGetString"). Single-threaded:
// the WebGPU device is preinitialized on the main thread by shell.html's preRun.

#include <cstdio>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <GL/glcorearb.h>

extern "C" {
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE mobilegl_webgl_create_context(const char* target,
                                                              const EmscriptenWebGLContextAttributes* attributes);
EMSCRIPTEN_RESULT mobilegl_webgl_make_context_current(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx);
void* mobilegl_get_proc_address(const char* name);
void mobilegl_present();
}

using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void (*)(GLbitfield);
using PFN_glGetString = const GLubyte* (*)(GLenum);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glAlphaFunc = void (*)(GLenum, GLclampf);

int main() {
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.depth = 1;
    attrs.alpha = 0;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = mobilegl_webgl_create_context("#canvas", &attrs);
    printf("[fpe-probe] create_context -> %d\n", (int)ctx);
    if (ctx == 0) { printf("[fpe-probe] FAIL: no context\n"); return 1; }
    EMSCRIPTEN_RESULT mc = mobilegl_webgl_make_context_current(ctx); // triggers sfpew_emscripten_init
    printf("[fpe-probe] make_current -> %d (SUCCESS=%d)\n", (int)mc, (int)EMSCRIPTEN_RESULT_SUCCESS);

    // Fixed-function-only entry point: only SFPEW provides this (MobileGL is core 3.3).
    void* pBegin = mobilegl_get_proc_address("glBegin");
    void* pEnable = mobilegl_get_proc_address("glMatrixMode");
    // Core entry points: resolved by SFPEW's fallback into MobileGL.
    void* pClear = mobilegl_get_proc_address("glClear");
    void* pClearColor = mobilegl_get_proc_address("glClearColor");
    void* pGetString = mobilegl_get_proc_address("glGetString");
    void* pGetIntegerv = mobilegl_get_proc_address("glGetIntegerv");
    void* pAlphaFunc = mobilegl_get_proc_address("glAlphaFunc");
    printf("[fpe-probe] proc: glBegin=%p glMatrixMode=%p glAlphaFunc=%p (FFP via SFPEW) | glClear=%p glClearColor=%p glGetString=%p glGetIntegerv=%p (core via MobileGL)\n",
           pBegin, pEnable, pAlphaFunc, pClear, pClearColor, pGetString, pGetIntegerv);

    bool routingOk = pBegin && pEnable && pAlphaFunc && pClear && pClearColor && pGetString && pGetIntegerv;
    printf("[fpe-probe] routing %s\n", routingOk ? "OK (SFPEW front + MobileGL fallback both resolve)" : "FAIL");

    bool profileOk = false;
    if (pGetIntegerv && pAlphaFunc) {
        auto glGetIntegerv_ = reinterpret_cast<PFN_glGetIntegerv>(pGetIntegerv);
        auto glAlphaFunc_ = reinterpret_cast<PFN_glAlphaFunc>(pAlphaFunc);
        GLint profile = 0;
        GLint flags = -1;
        glGetIntegerv_(GL_CONTEXT_PROFILE_MASK, &profile);
        glGetIntegerv_(GL_CONTEXT_FLAGS, &flags);
        profileOk = (profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0 &&
                    (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) == 0;
        printf("[fpe-probe] profile=0x%x flags=0x%x -> %s\n", profile, flags,
               profileOk ? "compatibility/non-forward OK" : "FAIL");
        // This is the first deprecated call Minecraft 1.16 makes after LWJGL capability setup.
        glAlphaFunc_(GL_GREATER, 0.1f);
    }

    if (pClearColor && pClear) {
        auto glClearColor_ = reinterpret_cast<PFN_glClearColor>(pClearColor);
        auto glClear_ = reinterpret_cast<PFN_glClear>(pClear);
        glClearColor_(0.1f, 0.9f, 0.2f, 1.0f);
        glClear_(GL_COLOR_BUFFER_BIT);
        mobilegl_present();
        printf("[fpe-probe] cleared+presented green through the FPE->MobileGL->WebGPU path\n");
    }
    if (pGetString) {
        auto glGetString_ = reinterpret_cast<PFN_glGetString>(pGetString);
        const GLubyte* v = glGetString_(GL_VERSION);
        printf("[fpe-probe] GL_VERSION=%s\n", v ? (const char*)v : "(null)");
    }
    const bool success = routingOk && profileOk;
    printf("[fpe-probe] %s\n", success ? "SUCCESS" : "FAILED");
    EM_ASM({ Module['__probeDone'] = true; });
    return success ? 0 : 2;
}
