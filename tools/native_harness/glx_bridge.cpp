#include <EGL/egl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>

namespace {
struct EglApi {
    EGLDisplay (*getDisplay)(EGLNativeDisplayType) = nullptr;
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (*swapBuffers)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface) = nullptr;
    __eglMustCastToProperFunctionPointerType (*getProcAddress)(const char *) = nullptr;
};

struct DisplayState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    std::unordered_map<GLXDrawable, EGLSurface> surfaces;
};

struct ContextState {
    Display *xDisplay = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
};

std::recursive_mutex gMutex;
void *gMobileGl = nullptr;
void *gSfpew = nullptr;
EglApi gEgl;
std::unordered_map<Display *, DisplayState> gDisplays;
std::unordered_map<GLXContext, ContextState> gContexts;
thread_local GLXContext gCurrentContext = nullptr;
thread_local GLXDrawable gCurrentDrawable = 0;
thread_local Display *gCurrentDisplay = nullptr;
int gFakeFbConfig;
unsigned long long gSwapCount = 0;

bool TraceEnabled() {
    const char *value = std::getenv("MOBILEGL_GLX_TRACE");
    return value && value[0] && std::strcmp(value, "0") != 0;
}

bool ProcTraceEnabled() {
    const char *value = std::getenv("MOBILEGL_GLX_TRACE_PROCS");
    return value && value[0] && std::strcmp(value, "0") != 0;
}

__attribute__((constructor)) void AnnounceBridgeLoad() {
    if (TraceEnabled()) {
        std::fprintf(stderr, "[mobilegl-glx] bridge loaded into process\n");
    }
}

template <typename T> bool Load(T &out, const char *name) {
    out = reinterpret_cast<T>(dlsym(gMobileGl, name));
    return out != nullptr;
}

bool EnsureMobileGl() {
    if (gMobileGl) return true;
    const char *library = std::getenv("MOBILEGL_LIBRARY");
    if (!library || !library[0]) library = "libMobileGL.so";
    gMobileGl = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    if (!gMobileGl) {
        std::fprintf(stderr, "[mobilegl-glx] dlopen(%s) failed: %s\n", library, dlerror());
        return false;
    }
    const bool loaded =
        Load(gEgl.getDisplay, "eglGetDisplay") && Load(gEgl.initialize, "eglInitialize") &&
        Load(gEgl.chooseConfig, "eglChooseConfig") && Load(gEgl.createContext, "eglCreateContext") &&
        Load(gEgl.createWindowSurface, "eglCreateWindowSurface") &&
        Load(gEgl.makeCurrent, "eglMakeCurrent") && Load(gEgl.swapBuffers, "eglSwapBuffers") &&
        Load(gEgl.destroyContext, "eglDestroyContext") &&
        Load(gEgl.destroySurface, "eglDestroySurface") &&
        Load(gEgl.getProcAddress, "eglGetProcAddress");
    if (loaded && TraceEnabled()) {
        std::fprintf(stderr, "[mobilegl-glx] loaded MobileGL from %s\n", library);
    }
    return loaded;
}

DisplayState *GetDisplayState(Display *xDisplay) {
    if (!EnsureMobileGl() || !xDisplay) return nullptr;
    auto [it, inserted] = gDisplays.try_emplace(xDisplay);
    auto &state = it->second;
    if (!inserted) return &state;
    state.display = gEgl.getDisplay(reinterpret_cast<EGLNativeDisplayType>(xDisplay));
    EGLint major = 0, minor = 0;
    if (state.display == EGL_NO_DISPLAY || !gEgl.initialize(state.display, &major, &minor)) return nullptr;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE,
    };
    EGLint count = 0;
    if (!gEgl.chooseConfig(state.display, attributes, &state.config, 1, &count) || count == 0) return nullptr;
    return &state;
}

EGLSurface GetSurface(Display *xDisplay, GLXDrawable drawable) {
    DisplayState *state = GetDisplayState(xDisplay);
    if (!state || drawable == 0) return EGL_NO_SURFACE;
    auto found = state->surfaces.find(drawable);
    if (found != state->surfaces.end()) return found->second;
    XWindowAttributes window{};
    XGetWindowAttributes(xDisplay, static_cast<Window>(drawable), &window);
    const EGLint attributes[] = {
        EGL_WIDTH, std::max(window.width, 1), EGL_HEIGHT, std::max(window.height, 1), EGL_NONE,
    };
    EGLSurface surface = gEgl.createWindowSurface(
        state->display, state->config, reinterpret_cast<EGLNativeWindowType>(drawable), attributes);
    if (surface != EGL_NO_SURFACE) {
        state->surfaces.emplace(drawable, surface);
        if (TraceEnabled()) {
            std::fprintf(stderr, "[mobilegl-glx] created %dx%d surface for drawable %lu\n",
                         std::max(window.width, 1), std::max(window.height, 1),
                         static_cast<unsigned long>(drawable));
        }
    }
    return surface;
}

void InitializeSfpew() {
    const char *frontend = std::getenv("MOBILEGL_FRONTEND");
    if (!frontend || std::strcmp(frontend, "sfpew") != 0 || !gEgl.getProcAddress) return;
    if (!gSfpew) {
        setenv("SFPEW_DEFER_INIT", "1", 1);
        const char *library = std::getenv("SFPEW_LIBRARY");
        if (!library || !library[0]) library = "libSimpleFPEWrapper.so";
        gSfpew = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    }
    using Init = int (*)(__eglMustCastToProperFunctionPointerType (*)(const char *));
    auto init = gSfpew ? reinterpret_cast<Init>(dlsym(gSfpew, "sfpew_init_with_proc")) : nullptr;
    // Passing MobileGL's eglGetProcAddress directly is unsafe in an interposed
    // process: its exported GL symbol references can be preempted by this GLX
    // bridge, which sends SFPEW's downstream calls back into SFPEW forever.
    // Resolve against the explicit MobileGL handle first to keep the lower and
    // upper halves of the frontend chain distinct.
    static const auto downstream = +[](const char *name) -> __eglMustCastToProperFunctionPointerType {
        if (!name || !EnsureMobileGl()) return nullptr;
        if (void *proc = dlsym(gMobileGl, name)) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(proc);
        }
        return gEgl.getProcAddress ? gEgl.getProcAddress(name) : nullptr;
    };
    if (init && !init(downstream)) {
        std::fprintf(stderr, "[mobilegl-glx] SFPEW downstream initialization failed\n");
    }
}

void *LookupGl(const char *name) {
    if (!name) return nullptr;
    const char *frontend = std::getenv("MOBILEGL_FRONTEND");
    if (frontend && std::strcmp(frontend, "sfpew") == 0) {
        if (!gSfpew) {
            setenv("SFPEW_DEFER_INIT", "1", 1);
            const char *library = std::getenv("SFPEW_LIBRARY");
            if (!library || !library[0]) library = "libSimpleFPEWrapper.so";
            gSfpew = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
        }
        if (gSfpew) if (void *proc = dlsym(gSfpew, name)) return proc;
    }
    if (EnsureMobileGl()) {
        if (void *proc = dlsym(gMobileGl, name)) return proc;
        void *proc = reinterpret_cast<void *>(gEgl.getProcAddress(name));
        if (!proc && TraceEnabled()) {
            std::fprintf(stderr, "[mobilegl-glx] unresolved GL procedure: %s\n", name);
        }
        return proc;
    }
    return nullptr;
}
} // namespace

extern "C" Bool glXQueryVersion(Display *, int *major, int *minor) {
    if (major) *major = 1;
    if (minor) *minor = 4;
    return True;
}

extern "C" Bool glXQueryExtension(Display *, int *errorBase, int *eventBase) {
    if (errorBase) *errorBase = 0;
    if (eventBase) *eventBase = 0;
    return True;
}

extern "C" const char *glXQueryExtensionsString(Display *, int) {
    return "GLX_ARB_create_context GLX_ARB_create_context_profile GLX_EXT_swap_control GLX_MESA_swap_control";
}
extern "C" const char *glXGetClientString(Display *, int) { return "MobileGL GLX-EGL bridge"; }
extern "C" const char *glXQueryServerString(Display *, int, int) { return "MobileGL GLX-EGL bridge"; }
extern "C" Bool glXIsDirect(Display *, GLXContext) { return True; }

extern "C" GLXFBConfig *glXChooseFBConfig(Display *, int, const int *, int *count) {
    auto *result = static_cast<GLXFBConfig *>(std::malloc(sizeof(GLXFBConfig)));
    if (result) *result = reinterpret_cast<GLXFBConfig>(&gFakeFbConfig);
    if (count) *count = result ? 1 : 0;
    return result;
}
extern "C" GLXFBConfig *glXGetFBConfigs(Display *display, int screen, int *count) {
    return glXChooseFBConfig(display, screen, nullptr, count);
}

extern "C" int glXGetFBConfigAttrib(Display *, GLXFBConfig, int attribute, int *value) {
    if (!value) return GLX_BAD_ATTRIBUTE;
    switch (attribute) {
    case GLX_FBCONFIG_ID: *value = 1; break;
    case GLX_VISUAL_ID: *value = 0; break;
    case GLX_DRAWABLE_TYPE: *value = GLX_WINDOW_BIT; break;
    case GLX_RENDER_TYPE: *value = GLX_RGBA_BIT; break;
    case GLX_DOUBLEBUFFER: *value = True; break;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *value = 8; break;
    case GLX_DEPTH_SIZE: *value = 24; break;
    case GLX_STENCIL_SIZE: *value = 8; break;
    default: *value = 0; break;
    }
    return Success;
}

extern "C" XVisualInfo *glXGetVisualFromFBConfig(Display *display, GLXFBConfig) {
    XVisualInfo query{};
    query.visualid = XVisualIDFromVisual(DefaultVisual(display, DefaultScreen(display)));
    int count = 0;
    return XGetVisualInfo(display, VisualIDMask, &query, &count);
}

extern "C" XVisualInfo *glXChooseVisual(Display *display, int screen, int *) {
    XVisualInfo query{};
    query.visualid = XVisualIDFromVisual(DefaultVisual(display, screen));
    int count = 0;
    return XGetVisualInfo(display, VisualIDMask, &query, &count);
}

extern "C" int glXGetConfig(Display *, XVisualInfo *, int attribute, int *value) {
    if (!value) return GLX_BAD_VALUE;
    switch (attribute) {
    case GLX_USE_GL: *value = True; break;
    case GLX_BUFFER_SIZE: *value = 32; break;
    case GLX_LEVEL: *value = 0; break;
    case GLX_RGBA: *value = True; break;
    case GLX_DOUBLEBUFFER: *value = True; break;
    case GLX_STEREO: *value = False; break;
    case GLX_AUX_BUFFERS: *value = 0; break;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *value = 8; break;
    case GLX_DEPTH_SIZE: *value = 24; break;
    case GLX_STENCIL_SIZE: *value = 8; break;
    case GLX_ACCUM_RED_SIZE: case GLX_ACCUM_GREEN_SIZE:
    case GLX_ACCUM_BLUE_SIZE: case GLX_ACCUM_ALPHA_SIZE: *value = 0; break;
    default: *value = 0; break;
    }
    return Success;
}

extern "C" GLXContext glXCreateContextAttribsARB(Display *display, GLXFBConfig, GLXContext share,
                                                   Bool, const int *attributes) {
    std::lock_guard lock(gMutex);
    DisplayState *state = GetDisplayState(display);
    if (!state) return nullptr;
    EGLint major = 3, minor = 3;
    for (const int *p = attributes; p && *p; p += 2) {
        if (p[0] == GLX_CONTEXT_MAJOR_VERSION_ARB) major = p[1];
        if (p[0] == GLX_CONTEXT_MINOR_VERSION_ARB) minor = p[1];
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, major,
                                        EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
    EGLContext shared = EGL_NO_CONTEXT;
    if (auto found = gContexts.find(share); found != gContexts.end()) shared = found->second.context;
    EGLContext egl = gEgl.createContext(state->display, state->config, shared, contextAttributes);
    if (egl == EGL_NO_CONTEXT) return nullptr;
    auto *token = new int(1);
    GLXContext context = reinterpret_cast<GLXContext>(token);
    gContexts.emplace(context, ContextState{display, egl});
    if (TraceEnabled()) {
        std::fprintf(stderr, "[mobilegl-glx] intercepted GLX context creation (%d.%d)\n", major, minor);
    }
    return context;
}

extern "C" GLXContext glXCreateNewContext(Display *display, GLXFBConfig config, int, GLXContext share, Bool direct) {
    const int attributes[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, None};
    return glXCreateContextAttribsARB(display, config, share, direct, attributes);
}
extern "C" GLXContext glXCreateContext(Display *display, XVisualInfo *, GLXContext share, Bool direct) {
    return glXCreateNewContext(display, reinterpret_cast<GLXFBConfig>(&gFakeFbConfig), GLX_RGBA_TYPE, share, direct);
}

extern "C" GLXWindow glXCreateWindow(Display *, GLXFBConfig, Window window, const int *) {
    return static_cast<GLXWindow>(window);
}
extern "C" void glXDestroyWindow(Display *display, GLXWindow window) {
    std::lock_guard lock(gMutex);
    auto state = gDisplays.find(display);
    if (state == gDisplays.end()) return;
    auto surface = state->second.surfaces.find(window);
    if (surface != state->second.surfaces.end()) {
        gEgl.destroySurface(state->second.display, surface->second);
        state->second.surfaces.erase(surface);
    }
}
extern "C" void glXQueryDrawable(Display *display, GLXDrawable drawable, int attribute, unsigned int *value) {
    if (!value) return;
    XWindowAttributes window{};
    if (display && XGetWindowAttributes(display, static_cast<Window>(drawable), &window)) {
        if (attribute == GLX_WIDTH) *value = static_cast<unsigned int>(window.width);
        else if (attribute == GLX_HEIGHT) *value = static_cast<unsigned int>(window.height);
        else *value = 0;
    } else {
        *value = 0;
    }
}
extern "C" void glXSelectEvent(Display *, GLXDrawable, unsigned long) {}
extern "C" void glXGetSelectedEvent(Display *, GLXDrawable, unsigned long *mask) { if (mask) *mask = 0; }

extern "C" Bool glXMakeCurrent(Display *display, GLXDrawable drawable, GLXContext context) {
    std::lock_guard lock(gMutex);
    if (!context) {
        DisplayState *state = GetDisplayState(display);
        Bool ok = state && gEgl.makeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ok) gCurrentContext = nullptr;
        return ok;
    }
    auto found = gContexts.find(context);
    if (found == gContexts.end()) return False;
    DisplayState *state = GetDisplayState(display);
    EGLSurface surface = GetSurface(display, drawable);
    Bool ok = state && surface != EGL_NO_SURFACE &&
              gEgl.makeCurrent(state->display, surface, surface, found->second.context);
    if (ok) {
        gCurrentContext = context;
        gCurrentDrawable = drawable;
        gCurrentDisplay = display;
        InitializeSfpew();
        if (TraceEnabled()) {
            std::fprintf(stderr, "[mobilegl-glx] MobileGL context is current on drawable %lu\n",
                         static_cast<unsigned long>(drawable));
        }
    }
    return ok;
}

extern "C" Bool glXMakeContextCurrent(Display *display, GLXDrawable draw, GLXDrawable read, GLXContext context) {
    (void)read;
    return glXMakeCurrent(display, draw, context);
}

extern "C" void glXSwapBuffers(Display *display, GLXDrawable drawable) {
    std::lock_guard lock(gMutex);
    DisplayState *state = GetDisplayState(display);
    EGLSurface surface = GetSurface(display, drawable);
    if (state && surface != EGL_NO_SURFACE) {
        const EGLBoolean ok = gEgl.swapBuffers(state->display, surface);
        ++gSwapCount;
        if (TraceEnabled() && (gSwapCount <= 3 || gSwapCount % 600 == 0)) {
            std::fprintf(stderr, "[mobilegl-glx] swap #%llu drawable %lu -> %s\n", gSwapCount,
                         static_cast<unsigned long>(drawable), ok ? "ok" : "failed");
        }
    }
}

extern "C" void glXDestroyContext(Display *display, GLXContext context) {
    std::lock_guard lock(gMutex);
    auto found = gContexts.find(context);
    DisplayState *state = GetDisplayState(display);
    if (found != gContexts.end()) {
        if (state) gEgl.destroyContext(state->display, found->second.context);
        delete reinterpret_cast<int *>(context);
        gContexts.erase(found);
    }
}

extern "C" GLXContext glXGetCurrentContext() { return gCurrentContext; }
extern "C" GLXDrawable glXGetCurrentDrawable() { return gCurrentDrawable; }
extern "C" GLXDrawable glXGetCurrentReadDrawable() { return gCurrentDrawable; }
extern "C" Display *glXGetCurrentDisplay() { return gCurrentDisplay; }
extern "C" void glXSwapIntervalEXT(Display *, GLXDrawable, int) {}
extern "C" int glXSwapIntervalMESA(unsigned int) { return 0; }
extern "C" int glXGetSwapIntervalMESA() { return 0; }
extern "C" int glXSwapIntervalSGI(int) { return 0; }

extern "C" const GLubyte *glGetString(GLenum name) {
    using Proc = const GLubyte *(*)(GLenum);
    Proc proc = reinterpret_cast<Proc>(LookupGl("glGetString"));
    const GLubyte *result = proc ? proc(name) : nullptr;
    if (TraceEnabled()) {
        std::fprintf(stderr, "[mobilegl-glx] glGetString(0x%x) -> %s\n", name,
                     result ? reinterpret_cast<const char *>(result) : "(null)");
    }
    return result;
}

extern "C" void glGetIntegerv(GLenum name, GLint *value) {
    using Proc = void (*)(GLenum, GLint *);
    Proc proc = reinterpret_cast<Proc>(LookupGl("glGetIntegerv"));
    if (proc) proc(name, value);
    // apitrace's core-profile feature discovery requires a non-empty indexed
    // extension list. DirectWebGPU currently reports zero extension strings.
    if (name == GL_NUM_EXTENSIONS && value && *value == 0) *value = 1;
    if (TraceEnabled() &&
        (name == GL_MAJOR_VERSION || name == GL_MINOR_VERSION || name == GL_NUM_EXTENSIONS)) {
        std::fprintf(stderr, "[mobilegl-glx] glGetIntegerv(0x%x) -> %d\n", name,
                     value ? *value : -1);
    }
}

extern "C" const GLubyte *glGetStringi(GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS && index == 0) {
        return reinterpret_cast<const GLubyte *>("GL_ARB_framebuffer_object");
    }
    using Proc = const GLubyte *(*)(GLenum, GLuint);
    Proc proc = reinterpret_cast<Proc>(LookupGl("glGetStringi"));
    const GLubyte *result = proc ? proc(name, index) : nullptr;
    if (TraceEnabled() && !result) {
        std::fprintf(stderr, "[mobilegl-glx] glGetStringi(0x%x, %u) -> null\n", name, index);
    }
    return result;
}

// LWJGL's Linux OpenGL FunctionProvider resolves the OpenGL 1.x ABI directly
// from its libGL.so.1 handle with dlsym; it does not use glXGetProcAddress for
// these symbols. Export the SFPEW surface from the bridge as well so both lookup
// paths reach the selected frontend. Without these trampolines, fixed-function
// calls resolve to MobileGL's core-profile dependency (or to null) and LWJGL
// aborts on the first glAlphaFunc call despite a current context.
#define MOBILEGL_FORWARD_VOID(symbol, signature, arguments)                                                           \
    extern "C" void symbol signature {                                                                                \
        using Proc = void (*) signature;                                                                              \
        auto proc = reinterpret_cast<Proc>(LookupGl(#symbol));                                                        \
        if (proc) proc arguments;                                                                                     \
    }
#define MOBILEGL_FORWARD_RET(result, fallback, symbol, signature, arguments)                                          \
    extern "C" result symbol signature {                                                                              \
        using Proc = result (*) signature;                                                                            \
        auto proc = reinterpret_cast<Proc>(LookupGl(#symbol));                                                        \
        return proc ? proc arguments : fallback;                                                                      \
    }

MOBILEGL_FORWARD_VOID(glActiveTexture, (GLenum texture), (texture))
MOBILEGL_FORWARD_VOID(glAlphaFunc, (GLenum func, GLclampf ref), (func, ref))
MOBILEGL_FORWARD_VOID(glBegin, (GLenum mode), (mode))
MOBILEGL_FORWARD_VOID(glCallList, (GLuint list), (list))
MOBILEGL_FORWARD_VOID(glCallLists, (GLsizei count, GLenum type, const GLvoid *lists), (count, type, lists))
MOBILEGL_FORWARD_VOID(glClientActiveTexture, (GLenum texture), (texture))
MOBILEGL_FORWARD_VOID(glColor3f, (GLfloat red, GLfloat green, GLfloat blue), (red, green, blue))
MOBILEGL_FORWARD_VOID(glColor4f, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha),
                      (red, green, blue, alpha))
MOBILEGL_FORWARD_VOID(glColorPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *pointer),
                      (size, type, stride, pointer))
MOBILEGL_FORWARD_VOID(glDeleteLists, (GLuint list, GLsizei range), (list, range))
MOBILEGL_FORWARD_VOID(glDisable, (GLenum capability), (capability))
MOBILEGL_FORWARD_VOID(glDisableClientState, (GLenum capability), (capability))
MOBILEGL_FORWARD_VOID(glDrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count))
MOBILEGL_FORWARD_VOID(glEnable, (GLenum capability), (capability))
MOBILEGL_FORWARD_VOID(glEnableClientState, (GLenum capability), (capability))
MOBILEGL_FORWARD_VOID(glEnd, (), ())
MOBILEGL_FORWARD_VOID(glEndList, (), ())
MOBILEGL_FORWARD_VOID(glFogf, (GLenum pname, GLfloat param), (pname, param))
MOBILEGL_FORWARD_VOID(glFogfv, (GLenum pname, const GLfloat *params), (pname, params))
MOBILEGL_FORWARD_VOID(glFogi, (GLenum pname, GLint param), (pname, param))
MOBILEGL_FORWARD_VOID(glFogiv, (GLenum pname, const GLint *params), (pname, params))
MOBILEGL_FORWARD_RET(GLuint, 0, glGenLists, (GLsizei range), (range))
MOBILEGL_FORWARD_VOID(glGetFloatv, (GLenum pname, GLfloat *params), (pname, params))
MOBILEGL_FORWARD_VOID(glIndexPointer, (GLenum type, GLsizei stride, const GLvoid *pointer),
                      (type, stride, pointer))
MOBILEGL_FORWARD_RET(GLboolean, GL_FALSE, glIsList, (GLuint list), (list))
MOBILEGL_FORWARD_VOID(glLightf, (GLenum light, GLenum pname, GLfloat param), (light, pname, param))
MOBILEGL_FORWARD_VOID(glLightfv, (GLenum light, GLenum pname, const GLfloat *params), (light, pname, params))
MOBILEGL_FORWARD_VOID(glLighti, (GLenum light, GLenum pname, GLint param), (light, pname, param))
MOBILEGL_FORWARD_VOID(glLightiv, (GLenum light, GLenum pname, const GLint *params), (light, pname, params))
MOBILEGL_FORWARD_VOID(glLightModelf, (GLenum pname, GLfloat param), (pname, param))
MOBILEGL_FORWARD_VOID(glLightModelfv, (GLenum pname, const GLfloat *params), (pname, params))
MOBILEGL_FORWARD_VOID(glLightModeli, (GLenum pname, GLint param), (pname, param))
MOBILEGL_FORWARD_VOID(glLightModeliv, (GLenum pname, const GLint *params), (pname, params))
MOBILEGL_FORWARD_VOID(glListBase, (GLuint base), (base))
MOBILEGL_FORWARD_VOID(glLoadIdentity, (), ())
MOBILEGL_FORWARD_VOID(glMatrixMode, (GLenum mode), (mode))
MOBILEGL_FORWARD_VOID(glMultMatrixf, (const GLfloat *matrix), (matrix))
MOBILEGL_FORWARD_VOID(glMultiTexCoord2f, (GLenum target, GLfloat s, GLfloat t), (target, s, t))
MOBILEGL_FORWARD_VOID(glMultiTexCoord4f,
                      (GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q), (target, s, t, r, q))
MOBILEGL_FORWARD_VOID(glNewList, (GLuint list, GLenum mode), (list, mode))
MOBILEGL_FORWARD_VOID(glNormal3f, (GLfloat x, GLfloat y, GLfloat z), (x, y, z))
MOBILEGL_FORWARD_VOID(glNormalPointer, (GLenum type, GLsizei stride, const GLvoid *pointer),
                      (type, stride, pointer))
MOBILEGL_FORWARD_VOID(glOrtho,
                      (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearValue,
                       GLdouble farValue),
                      (left, right, bottom, top, nearValue, farValue))
MOBILEGL_FORWARD_VOID(glOrthof,
                      (GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat nearValue, GLfloat farValue),
                      (left, right, bottom, top, nearValue, farValue))
MOBILEGL_FORWARD_VOID(glPopMatrix, (), ())
MOBILEGL_FORWARD_VOID(glPushMatrix, (), ())
MOBILEGL_FORWARD_VOID(glRotated, (GLdouble angle, GLdouble x, GLdouble y, GLdouble z), (angle, x, y, z))
MOBILEGL_FORWARD_VOID(glRotatef, (GLfloat angle, GLfloat x, GLfloat y, GLfloat z), (angle, x, y, z))
MOBILEGL_FORWARD_VOID(glScaled, (GLdouble x, GLdouble y, GLdouble z), (x, y, z))
MOBILEGL_FORWARD_VOID(glScalef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z))
MOBILEGL_FORWARD_VOID(glShadeModel, (GLenum mode), (mode))
MOBILEGL_FORWARD_VOID(glTexCoord2f, (GLfloat s, GLfloat t), (s, t))
MOBILEGL_FORWARD_VOID(glTexCoord4f, (GLfloat s, GLfloat t, GLfloat r, GLfloat q), (s, t, r, q))
MOBILEGL_FORWARD_VOID(glTexCoordPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *pointer),
                      (size, type, stride, pointer))
MOBILEGL_FORWARD_VOID(glTexEnvf, (GLenum target, GLenum pname, GLfloat param), (target, pname, param))
MOBILEGL_FORWARD_VOID(glTexEnvi, (GLenum target, GLenum pname, GLint param), (target, pname, param))
MOBILEGL_FORWARD_VOID(glTranslated, (GLdouble x, GLdouble y, GLdouble z), (x, y, z))
MOBILEGL_FORWARD_VOID(glTranslatef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z))
MOBILEGL_FORWARD_VOID(glVertex3f, (GLfloat x, GLfloat y, GLfloat z), (x, y, z))
MOBILEGL_FORWARD_VOID(glVertex4f, (GLfloat x, GLfloat y, GLfloat z, GLfloat w), (x, y, z, w))
MOBILEGL_FORWARD_VOID(glVertexPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *pointer),
                      (size, type, stride, pointer))

#undef MOBILEGL_FORWARD_RET
#undef MOBILEGL_FORWARD_VOID

extern "C" __GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name) {
    if (!name) return nullptr;
    const char *procName = reinterpret_cast<const char *>(name);
    if (std::strcmp(procName, "glGetString") == 0) {
        if (ProcTraceEnabled()) std::fprintf(stderr, "[mobilegl-glx] proc %s -> bridge\n", procName);
        return reinterpret_cast<__GLXextFuncPtr>(&glGetString);
    }
    if (std::strcmp(procName, "glGetIntegerv") == 0) {
        return reinterpret_cast<__GLXextFuncPtr>(&glGetIntegerv);
    }
    if (std::strcmp(procName, "glGetStringi") == 0) {
        return reinterpret_cast<__GLXextFuncPtr>(&glGetStringi);
    }
    if (std::strncmp(procName, "glX", 3) == 0) {
#define MOBILEGL_GLX_PROC(symbol)                                                                                     \
        if (std::strcmp(procName, #symbol) == 0) {                                                                    \
            if (ProcTraceEnabled()) std::fprintf(stderr, "[mobilegl-glx] proc %s -> bridge\n", procName);           \
            return reinterpret_cast<__GLXextFuncPtr>(&symbol);                                                       \
        }
        MOBILEGL_GLX_PROC(glXQueryVersion)
        MOBILEGL_GLX_PROC(glXQueryExtension)
        MOBILEGL_GLX_PROC(glXQueryExtensionsString)
        MOBILEGL_GLX_PROC(glXGetClientString)
        MOBILEGL_GLX_PROC(glXQueryServerString)
        MOBILEGL_GLX_PROC(glXIsDirect)
        MOBILEGL_GLX_PROC(glXChooseFBConfig)
        MOBILEGL_GLX_PROC(glXGetFBConfigs)
        MOBILEGL_GLX_PROC(glXGetFBConfigAttrib)
        MOBILEGL_GLX_PROC(glXGetVisualFromFBConfig)
        MOBILEGL_GLX_PROC(glXChooseVisual)
        MOBILEGL_GLX_PROC(glXGetConfig)
        MOBILEGL_GLX_PROC(glXCreateContextAttribsARB)
        MOBILEGL_GLX_PROC(glXCreateNewContext)
        MOBILEGL_GLX_PROC(glXCreateContext)
        MOBILEGL_GLX_PROC(glXCreateWindow)
        MOBILEGL_GLX_PROC(glXDestroyWindow)
        MOBILEGL_GLX_PROC(glXQueryDrawable)
        MOBILEGL_GLX_PROC(glXSelectEvent)
        MOBILEGL_GLX_PROC(glXGetSelectedEvent)
        MOBILEGL_GLX_PROC(glXMakeCurrent)
        MOBILEGL_GLX_PROC(glXMakeContextCurrent)
        MOBILEGL_GLX_PROC(glXSwapBuffers)
        MOBILEGL_GLX_PROC(glXDestroyContext)
        MOBILEGL_GLX_PROC(glXGetCurrentContext)
        MOBILEGL_GLX_PROC(glXGetCurrentDrawable)
        MOBILEGL_GLX_PROC(glXGetCurrentReadDrawable)
        MOBILEGL_GLX_PROC(glXGetCurrentDisplay)
        MOBILEGL_GLX_PROC(glXSwapIntervalEXT)
        MOBILEGL_GLX_PROC(glXSwapIntervalMESA)
        MOBILEGL_GLX_PROC(glXGetSwapIntervalMESA)
        MOBILEGL_GLX_PROC(glXSwapIntervalSGI)
        MOBILEGL_GLX_PROC(glXGetProcAddressARB)
        MOBILEGL_GLX_PROC(glXGetProcAddress)
#undef MOBILEGL_GLX_PROC
    }
    void *proc = LookupGl(procName);
    if (ProcTraceEnabled()) {
        std::fprintf(stderr, "[mobilegl-glx] proc %s -> %s\n", procName, proc ? "MobileGL" : "null");
    }
    return reinterpret_cast<__GLXextFuncPtr>(proc);
}
extern "C" __GLXextFuncPtr glXGetProcAddress(const GLubyte *name) { return glXGetProcAddressARB(name); }
