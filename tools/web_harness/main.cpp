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
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 1.0); }\n";
static const char* kFrag =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = texture(uTex, vUV); }\n";

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

static void SetupQuad() {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, CompileShader(GL_VERTEX_SHADER, kVert));
    glAttachShader(prog, CompileShader(GL_FRAGMENT_SHADER, kFrag));
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    printf("[harness] program link status=%d\n", linked);
    glUseProgram(prog);
    GLint texLoc = glGetUniformLocation(prog, "uTex");
    glUniform1i(texLoc, 0); // sampler uses texture unit 0
    printf("[harness] uTex location=%d\n", texLoc);

    // Interleaved position (vec3) + uv (vec2), stride 20 bytes.
    static const float verts[] = {
        -0.6f,  0.6f, 0.0f, 0.0f, 0.0f, // top-left
         0.6f,  0.6f, 0.0f, 1.0f, 0.0f, // top-right
         0.6f, -0.6f, 0.0f, 1.0f, 1.0f, // bottom-right
        -0.6f, -0.6f, 0.0f, 0.0f, 1.0f, // bottom-left
    };
    static const unsigned short indices[] = {0, 1, 2, 0, 2, 3};

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // A 2x2 RGBA8 texture (red, green, blue, yellow) so sampling is clearly visible.
    static const unsigned char texels[] = {
        255, 0,   0,   255,   0,   255, 0,   255,
        0,   0,   255, 255,   255, 255, 0,   255,
    };
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void Frame() {
    g_t += 0.016;
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void*)0);
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

    SetupQuad();
    printf("[harness] setup done; starting draw loop\n");
    emscripten_set_main_loop(Frame, 0, 1);
    return 0;
}
