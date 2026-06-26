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
static GLuint g_tex = 0;
static int g_frame = 0;

static GLuint g_texProg = 0, g_texVAO = 0;

// M5 render-state validation: a solid-color program + quads at known screen regions,
// drawn with depth test / blending so readback can verify the results.
static GLuint g_solidProg = 0, g_solidVAO = 0, g_colorLoc = 0;
static GLuint g_nearVBO = 0, g_farVBO = 0, g_blendBgVBO = 0, g_blendFgVBO = 0;

// M5 instancing validation: a small quad + a per-instance offset attribute (divisor 1).
static GLuint g_instProg = 0, g_instVAO = 0, g_instColorLoc = 0, g_instQuadVBO = 0, g_instOffVBO = 0;

// M5 render-to-texture validation: an FBO with a color texture, plus a quad to sample it.
static GLuint g_fbo = 0, g_fboTex = 0, g_fboInVBO = 0, g_fboSampleVAO = 0, g_fboSampleVBO = 0;

// M5 vertex-format validation: a quad with a normalized GL_UNSIGNED_BYTE color attribute.
static GLuint g_byteProg = 0, g_byteVAO = 0, g_byteVBO = 0;

// M5 multi-draw + buffer-re-upload validation (Sodium's main draw path).
static GLuint g_mdVAO = 0, g_mdVBO = 0, g_mdIBO = 0;

// M4 validation: synchronous readback of the framebuffer via JSPI. glFinish and
// glReadPixels suspend the wasm stack until the WebGPU copy/map completes; this is
// only legal when reached from a WebAssembly.promising entry, so the scheduler below
// wraps this export before calling it.
extern "C" EMSCRIPTEN_KEEPALIVE void harness_readback() {
    glFinish();
    unsigned char bg[4] = {0}, quad[4] = {0};
    // (10,10): outside the quad -> the clear color (~26,51,77).
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bg);
    // (180,180): inside the quad -> a saturated texture color.
    glReadPixels(180, 180, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, quad);
    printf("[harness] readback bg=(%d,%d,%d,%d) quad=(%d,%d,%d,%d)\n", bg[0], bg[1], bg[2], bg[3],
           quad[0], quad[1], quad[2], quad[3]);
    // glGetTexImage: read back the 2x2 texture's four texels (a rotation of the
    // red/green/blue/yellow palette).
    unsigned char tex[16] = {0};
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    printf("[harness] getteximage texels: (%d,%d,%d) (%d,%d,%d) (%d,%d,%d) (%d,%d,%d)\n", tex[0], tex[1],
           tex[2], tex[4], tex[5], tex[6], tex[8], tex[9], tex[10], tex[12], tex[13], tex[14]);
    // M5: depth test — left strip center. Far green was drawn after near red; with
    // GL_LESS it must be occluded, so this reads RED.
    unsigned char depthPix[4] = {0};
    glReadPixels(45, 256, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, depthPix);
    // M5: blend — right strip center. 50%-alpha red over opaque blue -> ~(128,0,128).
    unsigned char blendPix[4] = {0};
    glReadPixels(467, 256, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blendPix);
    printf("[harness] depthtest pixel=(%d,%d,%d) [expect ~255,0,0]  blend pixel=(%d,%d,%d) [expect "
           "~128,0,128]\n", depthPix[0], depthPix[1], depthPix[2], blendPix[0], blendPix[1], blendPix[2]);
}

// Wrap the export in WebAssembly.promising (needs module scope for wasmExports) and
// fire it once after a few frames have rendered.
EM_JS(void, harness_schedule_readback, (), {
    setTimeout(function() {
        try {
            if (typeof WebAssembly.promising !== 'function') {
                out('[harness] WebAssembly.promising unavailable (JSPI off?)');
                return;
            }
            // With WASM_ASYNC_COMPILATION=0, Module['_harness_readback'] is the raw
            // WebAssembly export (no lazy JS wrapper), which WebAssembly.promising needs.
            var fn = Module['_harness_readback'];
            var promising = WebAssembly.promising(fn);
            promising().then(function() { out('[harness] readback call returned'); })
                       .catch(function(e) { err('[harness] readback threw: ' + e); });
        } catch (e) {
            err('[harness] schedule_readback failed: ' + e);
        }
    }, 1500);
});

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

static const char* kSolidVert =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 1.0); }\n";
static const char* kSolidFrag =
    "#version 330 core\n"
    "uniform vec4 uColor;\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = uColor; }\n";

static const char* kInstVert =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"        // per-vertex (divisor 0)
    "layout(location=1) in vec2 aOffset;\n"     // per-instance (divisor 1)
    "void main() { gl_Position = vec4(aPos + aOffset, 0.05, 1.0); }\n";

static const char* kByteVert =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec4 aColor;\n"      // normalized GL_UNSIGNED_BYTE
    "out vec4 vColor;\n"
    "void main() { vColor = aColor; gl_Position = vec4(aPos, 0.05, 1.0); }\n";
static const char* kByteFrag =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = vColor; }\n";

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
    g_texProg = glCreateProgram();
    GLuint prog = g_texProg;
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

    GLuint vbo = 0, ebo = 0;
    glGenVertexArrays(1, &g_texVAO);
    glBindVertexArray(g_texVAO);
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
    glGenTextures(1, &g_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static GLuint MakeQuadVBO(float x0, float y0, float x1, float y1, float z) {
    const float v[] = {x0, y0, z, x1, y0, z, x1, y1, z, x0, y0, z, x1, y1, z, x0, y1, z};
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    return b;
}

static void SetupSolids() {
    g_solidProg = glCreateProgram();
    glAttachShader(g_solidProg, CompileShader(GL_VERTEX_SHADER, kSolidVert));
    glAttachShader(g_solidProg, CompileShader(GL_FRAGMENT_SHADER, kSolidFrag));
    glBindAttribLocation(g_solidProg, 0, "aPos");
    glLinkProgram(g_solidProg);
    g_colorLoc = glGetUniformLocation(g_solidProg, "uColor");
    glGenVertexArrays(1, &g_solidVAO);
    // Left strip (depth test): near (z=0.2) + far (z=0.8) overlap the same region.
    g_nearVBO = MakeQuadVBO(-0.95f, -0.3f, -0.70f, 0.3f, 0.2f);
    g_farVBO = MakeQuadVBO(-0.95f, -0.3f, -0.70f, 0.3f, 0.8f);
    // Right strip (blend): opaque blue bg + 50%-alpha red fg.
    g_blendBgVBO = MakeQuadVBO(0.70f, -0.3f, 0.95f, 0.3f, 0.1f);
    g_blendFgVBO = MakeQuadVBO(0.70f, -0.3f, 0.95f, 0.3f, 0.1f);
}

// Draws the M5 depth + blend test quads (called each frame, in fixed screen regions
// that don't overlap the textured quad).
static void DrawSolids() {
    glUseProgram(g_solidProg);
    glBindVertexArray(g_solidVAO);
    auto bindQuad = [](GLuint vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (const void*)0);
        glEnableVertexAttribArray(0);
    };
    // Depth test: draw NEAR red first, then FAR green over it. GL_LESS must keep red.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    bindQuad(g_nearVBO);
    glUniform4f(g_colorLoc, 1.0f, 0.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    bindQuad(g_farVBO);
    glUniform4f(g_colorLoc, 0.0f, 1.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6); // occluded where it overlaps the near red
    glDisable(GL_DEPTH_TEST);
    // Blend: opaque blue, then 50%-alpha red over it -> ~(128,0,128).
    bindQuad(g_blendBgVBO);
    glUniform4f(g_colorLoc, 0.0f, 0.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    bindQuad(g_blendFgVBO);
    glUniform4f(g_colorLoc, 1.0f, 0.0f, 0.0f, 0.5f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

static void SetupInstanced() {
    g_instProg = glCreateProgram();
    glAttachShader(g_instProg, CompileShader(GL_VERTEX_SHADER, kInstVert));
    glAttachShader(g_instProg, CompileShader(GL_FRAGMENT_SHADER, kSolidFrag));
    glBindAttribLocation(g_instProg, 0, "aPos");
    glBindAttribLocation(g_instProg, 1, "aOffset");
    glLinkProgram(g_instProg);
    g_instColorLoc = glGetUniformLocation(g_instProg, "uColor");
    glGenVertexArrays(1, &g_instVAO);
    glBindVertexArray(g_instVAO);
    // Small centered quad (per-vertex, vec2).
    const float quad[] = {-0.06f, -0.06f, 0.06f, -0.06f, 0.06f, 0.06f,
                          -0.06f, -0.06f, 0.06f, 0.06f, -0.06f, 0.06f};
    glGenBuffers(1, &g_instQuadVBO);
    glBindBuffer(GL_ARRAY_BUFFER, g_instQuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    // Four per-instance offsets along a bottom row (NDC).
    const float offsets[] = {-0.6f, -0.85f, -0.2f, -0.85f, 0.2f, -0.85f, 0.6f, -0.85f};
    glGenBuffers(1, &g_instOffVBO);
    glBindBuffer(GL_ARRAY_BUFFER, g_instOffVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(offsets), offsets, GL_STATIC_DRAW);
}

// M5 instancing: draw the small quad 4 times, each offset by a per-instance attribute.
static void DrawInstanced() {
    glUseProgram(g_instProg);
    glBindVertexArray(g_instVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_instQuadVBO);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_instOffVBO);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1); // advance once per instance
    glUniform4f(g_instColorLoc, 0.0f, 1.0f, 1.0f, 1.0f); // cyan
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 4);
}

static void SetupByteColor() {
    g_byteProg = glCreateProgram();
    glAttachShader(g_byteProg, CompileShader(GL_VERTEX_SHADER, kByteVert));
    glAttachShader(g_byteProg, CompileShader(GL_FRAGMENT_SHADER, kByteFrag));
    glBindAttribLocation(g_byteProg, 0, "aPos");
    glBindAttribLocation(g_byteProg, 1, "aColor");
    glLinkProgram(g_byteProg);
    // Interleaved: vec2 float pos (8B) + 4x uint8 color (4B) = 12B stride.
    struct V { float x, y; unsigned char r, g, b, a; };
    static const V verts[6] = {
        {-0.92f, 0.66f, 255, 128, 0, 255}, {-0.55f, 0.66f, 255, 128, 0, 255},
        {-0.55f, 0.92f, 255, 128, 0, 255}, {-0.92f, 0.66f, 255, 128, 0, 255},
        {-0.55f, 0.92f, 255, 128, 0, 255}, {-0.92f, 0.92f, 255, 128, 0, 255},
    };
    glGenVertexArrays(1, &g_byteVAO);
    glBindVertexArray(g_byteVAO);
    glGenBuffers(1, &g_byteVBO);
    glBindBuffer(GL_ARRAY_BUFFER, g_byteVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
}

// Draw the byte-color quad (top-left): a normalized GL_UNSIGNED_BYTE color attribute
// should render as orange (255,128,0).
static void DrawByteColor() {
    glUseProgram(g_byteProg);
    glBindVertexArray(g_byteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_byteVBO);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 12, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 12, (const void*)8);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void SetupMultiDraw() {
    // 8 vertices = two quads (A at baseVertex 0, B at baseVertex 4) in the top-right.
    glGenVertexArrays(1, &g_mdVAO);
    glBindVertexArray(g_mdVAO);
    glGenBuffers(1, &g_mdVBO);
    glBindBuffer(GL_ARRAY_BUFFER, g_mdVBO);
    glBufferData(GL_ARRAY_BUFFER, 8 * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW); // filled each frame
    static const unsigned short idx[] = {0, 1, 2, 0, 2, 3};
    glGenBuffers(1, &g_mdIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_mdIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
}

// glMultiDrawElementsBaseVertex: two quads from one VBO/IBO via baseVertex. The VBO is
// re-uploaded each frame with an animated Y so this also exercises buffer re-upload.
static void DrawMultiDraw() {
    const float dy = 0.04f * (float)((g_frame / 30) % 2); // small periodic shift
    float v[24];
    auto quad = [&](int base, float x0, float x1, float y0, float y1) {
        const float p[12] = {x0, y0, 0.05f, x1, y0, 0.05f, x1, y1, 0.05f, x0, y1, 0.05f};
        for (int i = 0; i < 12; ++i) v[base * 3 + i] = p[i];
    };
    quad(0, 0.55f, 0.72f, 0.66f + dy, 0.86f + dy); // quad A
    quad(4, 0.74f, 0.92f, 0.66f + dy, 0.86f + dy); // quad B (baseVertex 4)
    glBindBuffer(GL_ARRAY_BUFFER, g_mdVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW); // re-upload -> bumps change serial
    glUseProgram(g_solidProg);
    glBindVertexArray(g_mdVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_mdVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_mdIBO);
    glUniform4f(g_colorLoc, 1.0f, 0.5f, 1.0f, 1.0f); // pink
    const GLsizei count[2] = {6, 6};
    const void* const indices[2] = {(const void*)0, (const void*)0};
    const GLint basevertex[2] = {0, 4};
    glMultiDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices, 2, basevertex);
}

static void SetupFBO() {
    // Color texture (no data; rendered into), no depth attachment.
    glGenTextures(1, &g_fboTex);
    glBindTexture(GL_TEXTURE_2D, g_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fboTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // A quad covering the FBO's right half (drawn into the FBO with the solid program).
    g_fboInVBO = MakeQuadVBO(0.0f, -1.0f, 1.0f, 1.0f, 0.1f);
    // A screen quad (top-center) that samples the FBO texture (interleaved pos+uv).
    static const float sv[] = {
        -0.25f, 0.65f, 0.0f, 0.0f, 1.0f, 0.25f, 0.65f, 0.0f, 1.0f, 1.0f,
        0.25f,  0.92f, 0.0f, 1.0f, 0.0f, -0.25f, 0.65f, 0.0f, 0.0f, 1.0f,
        0.25f,  0.92f, 0.0f, 1.0f, 0.0f, -0.25f, 0.92f, 0.0f, 0.0f, 0.0f,
    };
    glGenVertexArrays(1, &g_fboSampleVAO);
    glBindVertexArray(g_fboSampleVAO);
    glGenBuffers(1, &g_fboSampleVBO);
    glBindBuffer(GL_ARRAY_BUFFER, g_fboSampleVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sv), sv, GL_STATIC_DRAW);
}

// Render into the FBO color texture: clear blue, draw a green quad over the right half.
static void RenderToFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, 128, 128);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_solidProg);
    glBindVertexArray(g_solidVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_fboInVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 0);
    glUniform4f(g_colorLoc, 0.0f, 1.0f, 0.0f, 1.0f); // green
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 512, 512);
}

// Draw the screen quad (top-center) sampling the FBO texture: should show blue|green.
static void DrawFBOSample() {
    glUseProgram(g_texProg);
    glBindVertexArray(g_fboSampleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_fboSampleVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_fboTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, g_tex); // restore the main texture for next frame
}

// M5: glBlitFramebuffer the FBO (128x128 blue|green) to the bottom-left of the screen.
static void DrawBlitTest() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 128, 128, 10, 10, 138, 138, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // restore default for subsequent frames
}

static void Frame() {
    g_t += 0.016;
    // Every ~40 frames, rotate the 2x2 texel colors via glTexSubImage2D to exercise
    // the re-upload-on-dirty path (the quadrant colors should cycle).
    if (g_frame % 40 == 0) {
        static const unsigned char palette[4][4] = {
            {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255},
        };
        const int shift = (g_frame / 40) % 4;
        unsigned char texels[16];
        for (int i = 0; i < 4; ++i) {
            const int c = (i + shift) % 4;
            texels[i * 4 + 0] = palette[c][0];
            texels[i * 4 + 1] = palette[c][1];
            texels[i * 4 + 2] = palette[c][2];
            texels[i * 4 + 3] = palette[c][3];
        }
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    }
    ++g_frame;
    // M5: render-to-texture into the FBO first (its result is sampled below).
    RenderToFBO();
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // Textured quad (center): rebind its program/VAO since DrawSolids changes them.
    glUseProgram(g_texProg);
    glBindVertexArray(g_texVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void*)0);
    // M5: depth-test + blend validation quads (left/right strips).
    DrawSolids();
    // M5: instanced draw (bottom row of 4 cyan quads).
    DrawInstanced();
    // M5: sample the FBO texture onto a top-center quad (shows blue|green).
    DrawFBOSample();
    // M5: normalized-byte vertex color attribute (top-left orange quad).
    DrawByteColor();
    // M5: multi-draw + buffer re-upload (two pink quads, top-right).
    DrawMultiDraw();
    // M5: blit the FBO to the bottom-left corner (after all draws so it isn't overdrawn).
    DrawBlitTest();
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
    SetupSolids();
    SetupInstanced();
    SetupFBO();
    SetupByteColor();
    SetupMultiDraw();
    printf("[harness] setup done; starting draw loop\n");
    harness_schedule_readback(); // M4: fire a JSPI readback after ~1.5s of frames
    emscripten_set_main_loop(Frame, 0, 1);
    return 0;
}
