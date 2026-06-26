// MobileGL standalone web harness: drives the real GL/EGL frontend so the
// DirectWebGPU backend clears the canvas (M2a) and draws a triangle (M2b).
#include <cstdio>
#include <cmath>

#include <emscripten.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

namespace MobileGL { void Initialize(); }

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static double g_t = 0.0;

static const char* kVert =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 1.0); }\n";
static const char* kFrag =
    "#version 330 core\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = vec4(1.0, 0.6, 0.1, 1.0); }\n";

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        printf("[harness] shader compile failed: %s\n", log);
    }
    return s;
}

static void SetupTriangle() {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, CompileShader(GL_VERTEX_SHADER, kVert));
    glAttachShader(prog, CompileShader(GL_FRAGMENT_SHADER, kFrag));
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    printf("[harness] program link status=%d\n", linked);
    glUseProgram(prog);

    static const float verts[] = {
         0.0f,  0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
}

static void Frame() {
    g_t += 0.016;
    glClearColor(0.1f, 0.2f, 0.5f + 0.3f * std::sin(g_t), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
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

    g_surf = eglCreateWindowSurface(g_dpy, config, (EGLNativeWindowType)1, nullptr);
    if (g_surf == EGL_NO_SURFACE) {
        printf("[harness] eglCreateWindowSurface FAILED\n");
        return 1;
    }
    eglMakeCurrent(g_dpy, g_surf, g_surf, ctx);

    SetupTriangle();
    printf("[harness] setup done; starting draw loop\n");
    emscripten_set_main_loop(Frame, 0, 1);
    return 0;
}
