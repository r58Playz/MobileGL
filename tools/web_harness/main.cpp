// MobileGL standalone web harness (M3): drives the real GL/EGL frontend so the
// DirectWebGPU backend clears a canvas. Validates M2a end-to-end in a browser.
#include <cstdio>
#include <cmath>

#include <emscripten.h>
#include <EGL/egl.h>
#include <GL/gl.h>

// MobileGL has no auto-init constructor pulled into a statically-linked exe unless
// something references Init.cpp; call Initialize() explicitly (it is idempotent).
namespace MobileGL { void Initialize(); }

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static double g_t = 0.0;

static void Frame() {
    g_t += 0.016;
    glClearColor(0.5f + 0.5f * std::sin(g_t), 0.2f, 0.5f + 0.5f * std::cos(g_t), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(g_dpy, g_surf);
}

int main() {
    MobileGL::Initialize();

    static const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, nullptr, nullptr);
    EGLConfig config;
    EGLint numConfig = 0;
    eglChooseConfig(g_dpy, configAttribs, &config, 1, &numConfig);
    EGLContext ctx = eglCreateContext(g_dpy, config, EGL_NO_CONTEXT, nullptr);

    // Non-null native window; the DirectWebGPU backend targets "#canvas".
    g_surf = eglCreateWindowSurface(g_dpy, config, (EGLNativeWindowType)1, nullptr);
    if (g_surf == EGL_NO_SURFACE) {
        printf("[harness] eglCreateWindowSurface FAILED\n");
        return 1;
    }
    eglMakeCurrent(g_dpy, g_surf, g_surf, ctx);
    printf("[harness] EGL ready; starting clear loop\n");

    emscripten_set_main_loop(Frame, 0, 1);
    return 0;
}
