// MobileGL browser-side apitrace replayer: replays a recorded apitrace GL trace
// through the real MobileGL GL/EGL frontend onto the DirectWebGPU backend, then reads
// back the framebuffer at a target call for golden-image comparison.
//
// Scope: the GL functions the Minecraft-Sodium-in-world fixture actually uses (decoded
// from the trace). Object ids and uniform locations are remapped trace->replay. For a
// snapshot replay we skip SwapBuffers/Present and read the offscreen at the target call.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#include "trace_parser.hpp"
#include "trace_file.hpp"

namespace MobileGL { void Initialize(); }

#include <fstream>

// Stub the non-snappy apitrace file backends (the fixtures are snappy-compressed) and
// provide a snappy-only createForRead so we don't pull in zstd/zlib/brotli for wasm.
namespace trace {
    File *File::createZLib(void) { return nullptr; }
    File *File::createBrotli(void) { return nullptr; }
    File *File::createZstd(void) { return nullptr; }
    File *File::createZstdSeekable(void) { return nullptr; }

    File *File::createForRead(const char *filename) {
        std::ifstream stream(filename, std::ifstream::binary | std::ifstream::in);
        if (!stream.is_open()) return nullptr;
        unsigned char m0 = 0, m1 = 0;
        stream >> m0 >> m1;
        stream.close();
        if (!(m0 == 'a' && m1 == 't')) { // apitrace snappy signature
            printf("[replay] unsupported trace compression (magic %02x %02x)\n", m0, m1);
            return nullptr;
        }
        File *file = File::createSnappy();
        if (!file || !file->open(filename)) { delete file; return nullptr; }
        return file;
    }
}

// ---- replay state ------------------------------------------------------------------
namespace {
    const char* kTracePath = "/trace.trace";
    int g_width = 854, g_height = 480;
    long long g_targetCall = 923340;

    EGLDisplay g_dpy = EGL_NO_DISPLAY;
    EGLSurface g_surf = EGL_NO_SURFACE;

    // trace-id -> replay-id maps for each GL object class.
    std::unordered_map<unsigned, GLuint> g_buf, g_tex, g_fbo, g_vao, g_prog, g_shader, g_rbo;
    // (traceProgram<<32 | traceLocation) -> replay uniform location.
    std::unordered_map<unsigned long long, GLint> g_uniform;
    unsigned g_curTraceProg = 0; // active program (trace id), for uniform-location remap
    // Debug toggle (set Module.skipUniforms=true before replay): skips all glUniform*
    // so the rest of the pipeline can be exercised while MobileGL's frontend
    // uniform-reflection bug on these shaders is outstanding.
    bool g_skipUniforms = false;

    std::vector<uint8_t> g_pixels; // RGBA readback at the target call
    bool g_captured = false;
    long long g_callsReplayed = 0;

    GLuint remap(const std::unordered_map<unsigned, GLuint>& m, unsigned traceName) {
        if (traceName == 0) return 0;
        auto it = m.find(traceName);
        return it != m.end() ? it->second : traceName; // fall back to identity
    }

    // Generate `n` names into the class map from the trace's returned-names array.
    void genNames(std::unordered_map<unsigned, GLuint>& m, trace::Call* c, int nArg, int arrArg,
                  void (*genfn)(GLsizei, GLuint*)) {
        const GLsizei n = static_cast<GLsizei>(c->arg(nArg).toSInt());
        if (n <= 0) return;
        const trace::Array* a = c->arg(arrArg).toArray();
        if (!a) return;
        std::vector<GLuint> mine(n);
        genfn(n, mine.data());
        for (GLsizei i = 0; i < n && i < (GLsizei)a->size(); ++i) {
            m[(unsigned)a->values[i]->toUInt()] = mine[i];
        }
    }

    // Float array from a Blob (raw bytes) or Array argument.
    const float* floatPtr(trace::Value& v, std::vector<float>& scratch) {
        if (const trace::Blob* b = v.toBlob()) return reinterpret_cast<const float*>(b->buf);
        if (const trace::Array* a = v.toArray()) {
            scratch.resize(a->size());
            for (size_t i = 0; i < a->size(); ++i) scratch[i] = a->values[i]->toFloat();
            return scratch.data();
        }
        return nullptr;
    }
    const int* intPtr(trace::Value& v, std::vector<int>& scratch) {
        if (const trace::Blob* b = v.toBlob()) return reinterpret_cast<const int*>(b->buf);
        if (const trace::Array* a = v.toArray()) {
            scratch.resize(a->size());
            for (size_t i = 0; i < a->size(); ++i) scratch[i] = (int)a->values[i]->toSInt();
            return scratch.data();
        }
        return nullptr;
    }
    // Blob/pointer data (texture pixels, buffer data); may be null.
    const void* blobPtr(trace::Value& v, size_t& outSize) {
        if (const trace::Blob* b = v.toBlob()) { outSize = b->size; return b->buf; }
        outSize = 0;
        return nullptr;
    }
} // namespace

// Dispatch one trace call to the matching GL function (with remapping).
static void dispatch(trace::Call* c) {
    const char* n = c->name();
    auto AU = [&](int i) { return (GLuint)c->arg(i).toUInt(); };
    auto AS = [&](int i) { return (GLint)c->arg(i).toSInt(); };
    auto AF = [&](int i) { return (GLfloat)c->arg(i).toFloat(); };
    auto AE = [&](int i) { return (GLenum)c->arg(i).toUInt(); };

    // --- object lifecycle (remapped) ---
    if (!strcmp(n, "glGenBuffers")) { genNames(g_buf, c, 0, 1, glGenBuffers); return; }
    if (!strcmp(n, "glGenTextures")) { genNames(g_tex, c, 0, 1, glGenTextures); return; }
    if (!strcmp(n, "glGenFramebuffers")) { genNames(g_fbo, c, 0, 1, glGenFramebuffers); return; }
    if (!strcmp(n, "glGenVertexArrays")) { genNames(g_vao, c, 0, 1, glGenVertexArrays); return; }
    if (!strcmp(n, "glBindBuffer")) { glBindBuffer(AE(0), remap(g_buf, AU(1))); return; }
    if (!strcmp(n, "glBindTexture")) { glBindTexture(AE(0), remap(g_tex, AU(1))); return; }
    if (!strcmp(n, "glBindVertexArray")) { glBindVertexArray(remap(g_vao, AU(0))); return; }
    if (!strcmp(n, "glBindFramebuffer")) { glBindFramebuffer(AE(0), remap(g_fbo, AU(1))); return; }
    if (!strcmp(n, "glCreateProgram")) {
        if (c->ret) g_prog[(unsigned)c->ret->toUInt()] = glCreateProgram();
        return;
    }
    if (!strcmp(n, "glCreateShader")) {
        if (c->ret) g_shader[(unsigned)c->ret->toUInt()] = glCreateShader(AE(0));
        return;
    }
    if (!strcmp(n, "glAttachShader")) { glAttachShader(remap(g_prog, AU(0)), remap(g_shader, AU(1))); return; }
    if (!strcmp(n, "glLinkProgram")) { glLinkProgram(remap(g_prog, AU(0))); return; }
    if (!strcmp(n, "glUseProgram")) {
        g_curTraceProg = AU(0);
        glUseProgram(remap(g_prog, AU(0)));
        return;
    }
    if (!strcmp(n, "glCompileShader")) { glCompileShader(remap(g_shader, AU(0))); return; }
    if (!strcmp(n, "glShaderSource")) {
        const GLuint sh = remap(g_shader, AU(0));
        const trace::Array* a = c->arg(2).toArray(); // array of source strings
        if (!a) return;
        std::vector<const char*> strs(a->size());
        for (size_t i = 0; i < a->size(); ++i) strs[i] = a->values[i]->toString();
        glShaderSource(sh, (GLsizei)a->size(), strs.data(), nullptr);
        return;
    }
    if (!strcmp(n, "glBindAttribLocation")) {
        glBindAttribLocation(remap(g_prog, AU(0)), AU(1), c->arg(2).toString());
        return;
    }
    if (!strcmp(n, "glBindFragDataLocation")) {
        glBindFragDataLocation(remap(g_prog, AU(0)), AU(1), c->arg(2).toString());
        return;
    }
    if (!strcmp(n, "glGetUniformLocation")) {
        const unsigned tp = AU(0);
        const GLint replayLoc = glGetUniformLocation(remap(g_prog, tp), c->arg(1).toString());
        if (c->ret) {
            const GLint traceLoc = (GLint)c->ret->toSInt();
            g_uniform[((unsigned long long)tp << 32) | (unsigned)traceLoc] = replayLoc;
        }
        return;
    }
    if (!strcmp(n, "glDeleteBuffers") || !strcmp(n, "glDeleteTextures") ||
        !strcmp(n, "glDeleteFramebuffers")) {
        return; // deletes: leave resources (harmless for a one-shot replay)
    }
    if (!strcmp(n, "glDeleteProgram") || !strcmp(n, "glDeleteShader")) return;

    if (g_skipUniforms && !strncmp(n, "glUniform", 9)) return;
    // --- uniforms (location remapped against the active program) ---
    auto loc = [&]() -> GLint {
        const GLint traceLoc = AS(0);
        if (traceLoc < 0) return -1;
        auto it = g_uniform.find(((unsigned long long)g_curTraceProg << 32) | (unsigned)traceLoc);
        return it != g_uniform.end() ? it->second : -1;
    };
    if (!strcmp(n, "glUniform1i")) { glUniform1i(loc(), AS(1)); return; }
    if (!strcmp(n, "glUniform1f")) { glUniform1f(loc(), AF(1)); return; }
    if (!strcmp(n, "glUniform2f")) { glUniform2f(loc(), AF(1), AF(2)); return; }
    if (!strcmp(n, "glUniform3f")) { glUniform3f(loc(), AF(1), AF(2), AF(3)); return; }
    if (!strcmp(n, "glUniform4f")) { glUniform4f(loc(), AF(1), AF(2), AF(3), AF(4)); return; }
    if (!strcmp(n, "glUniform1iv")) {
        std::vector<int> s; const int* p = intPtr(c->arg(2), s);
        if (p) glUniform1iv(loc(), AS(1), p);
        return;
    }
    if (!strcmp(n, "glUniform1fv") || !strcmp(n, "glUniform2fv") || !strcmp(n, "glUniform3fv") ||
        !strcmp(n, "glUniform4fv")) {
        std::vector<float> s; const float* p = floatPtr(c->arg(2), s);
        if (!p) return;
        const GLsizei cnt = (GLsizei)AS(1);
        switch (n[9]) { // "glUniform" is 9 chars; the component count digit is at index 9
        case '1': glUniform1fv(loc(), cnt, p); break;
        case '2': glUniform2fv(loc(), cnt, p); break;
        case '3': glUniform3fv(loc(), cnt, p); break;
        default: glUniform4fv(loc(), cnt, p); break;
        }
        return;
    }
    if (!strcmp(n, "glUniformMatrix4fv")) {
        std::vector<float> s; const float* p = floatPtr(c->arg(3), s);
        if (p) glUniformMatrix4fv(loc(), AS(1), (GLboolean)c->arg(2).toBool(), p);
        return;
    }

    // --- buffers / vertex arrays ---
    if (!strcmp(n, "glBufferData")) {
        size_t sz = 0; const void* d = blobPtr(c->arg(2), sz);
        glBufferData(AE(0), (GLsizeiptr)c->arg(1).toUInt(), d, AE(3));
        return;
    }
    if (!strcmp(n, "glBufferStorage")) {
        size_t sz = 0; const void* d = blobPtr(c->arg(2), sz);
        glBufferStorage(AE(0), (GLsizeiptr)c->arg(1).toUInt(), d, (GLbitfield)c->arg(3).toUInt());
        return;
    }
    if (!strcmp(n, "glCopyBufferSubData")) {
        glCopyBufferSubData(AE(0), AE(1), (GLintptr)c->arg(2).toUInt(), (GLintptr)c->arg(3).toUInt(),
                            (GLsizeiptr)c->arg(4).toUInt());
        return;
    }
    if (!strcmp(n, "glEnableVertexAttribArray")) { glEnableVertexAttribArray(AU(0)); return; }
    if (!strcmp(n, "glVertexAttribPointer")) {
        glVertexAttribPointer(AU(0), AS(1), AE(2), (GLboolean)c->arg(3).toBool(), AS(4),
                              (const void*)(uintptr_t)c->arg(5).toUIntPtr());
        return;
    }
    if (!strcmp(n, "glVertexAttribIPointer")) {
        glVertexAttribIPointer(AU(0), AS(1), AE(2), AS(3), (const void*)(uintptr_t)c->arg(4).toUIntPtr());
        return;
    }

    // --- textures ---
    if (!strcmp(n, "glActiveTexture")) { glActiveTexture(AE(0)); return; }
    if (!strcmp(n, "glTexParameteri")) { glTexParameteri(AE(0), AE(1), AS(2)); return; }
    if (!strcmp(n, "glTexParameterf")) { glTexParameterf(AE(0), AE(1), AF(2)); return; }
    if (!strcmp(n, "glPixelStorei")) { glPixelStorei(AE(0), AS(1)); return; }
    if (!strcmp(n, "glTexImage2D")) {
        size_t sz = 0; const void* d = blobPtr(c->arg(8), sz);
        glTexImage2D(AE(0), AS(1), AS(2), AS(3), AS(4), AS(5), AE(6), AE(7), d);
        return;
    }
    if (!strcmp(n, "glTexSubImage2D")) {
        size_t sz = 0; const void* d = blobPtr(c->arg(8), sz);
        glTexSubImage2D(AE(0), AS(1), AS(2), AS(3), AS(4), AS(5), AE(6), AE(7), d);
        return;
    }

    // --- framebuffers ---
    if (!strcmp(n, "glFramebufferTexture2D")) {
        glFramebufferTexture2D(AE(0), AE(1), AE(2), remap(g_tex, AU(3)), AS(4));
        return;
    }
    if (!strcmp(n, "glBlitFramebuffer")) {
        glBlitFramebuffer(AS(0), AS(1), AS(2), AS(3), AS(4), AS(5), AS(6), AS(7),
                          (GLbitfield)c->arg(8).toUInt(), AE(9));
        return;
    }
    if (!strcmp(n, "glCheckFramebufferStatus")) return; // query, no side effect

    // --- render state ---
    if (!strcmp(n, "glEnable")) { glEnable(AE(0)); return; }
    if (!strcmp(n, "glDisable")) { glDisable(AE(0)); return; }
    if (!strcmp(n, "glDepthFunc")) { glDepthFunc(AE(0)); return; }
    if (!strcmp(n, "glDepthMask")) { glDepthMask((GLboolean)c->arg(0).toBool()); return; }
    if (!strcmp(n, "glBlendFunc")) { glBlendFunc(AE(0), AE(1)); return; }
    if (!strcmp(n, "glBlendFuncSeparate")) { glBlendFuncSeparate(AE(0), AE(1), AE(2), AE(3)); return; }
    if (!strcmp(n, "glColorMask")) {
        glColorMask((GLboolean)c->arg(0).toBool(), (GLboolean)c->arg(1).toBool(),
                    (GLboolean)c->arg(2).toBool(), (GLboolean)c->arg(3).toBool());
        return;
    }
    if (!strcmp(n, "glViewport")) { glViewport(AS(0), AS(1), AS(2), AS(3)); return; }
    if (!strcmp(n, "glScissor")) { glScissor(AS(0), AS(1), AS(2), AS(3)); return; }
    if (!strcmp(n, "glClearColor")) { glClearColor(AF(0), AF(1), AF(2), AF(3)); return; }
    if (!strcmp(n, "glClearDepth")) { glClearDepth(c->arg(0).toDouble()); return; }
    if (!strcmp(n, "glClear")) { glClear((GLbitfield)c->arg(0).toUInt()); return; }

    // --- draws ---
    if (!strcmp(n, "glDrawElements")) {
        glDrawElements(AE(0), AS(1), AE(2), (const void*)(uintptr_t)c->arg(3).toUIntPtr());
        return;
    }
    if (!strcmp(n, "glMultiDrawElementsBaseVertex")) {
        const GLsizei dc = (GLsizei)AS(4);
        const trace::Array* ca = c->arg(1).toArray();   // counts
        const trace::Array* ia = c->arg(3).toArray();   // index byte-offsets (pointers)
        const trace::Array* ba = c->arg(5).toArray();   // basevertex
        if (!ca || !ia) return;
        std::vector<GLsizei> counts(dc);
        std::vector<const void*> idx(dc);
        std::vector<GLint> bv(dc, 0);
        for (GLsizei i = 0; i < dc; ++i) {
            counts[i] = (GLsizei)ca->values[i]->toSInt();
            idx[i] = (const void*)(uintptr_t)ia->values[i]->toUIntPtr();
            if (ba && i < (GLsizei)ba->size()) bv[i] = (GLint)ba->values[i]->toSInt();
        }
        glMultiDrawElementsBaseVertex(AE(0), counts.data(), AE(2), idx.data(), dc, bv.data());
        return;
    }

    // Everything else (queries, sync, mapping, debug, swapbuffers) is skipped: it has no
    // bearing on the captured frame for this fixture.
}

// Replays the trace up to the target call, then reads back the framebuffer.
extern "C" EMSCRIPTEN_KEEPALIVE void replay_run() {
    trace::Parser parser;
    if (!parser.open(kTracePath)) {
        printf("[replay] failed to open %s\n", kTracePath);
        return;
    }
    g_skipUniforms = EM_ASM_INT({ return Module['skipUniforms'] ? 1 : 0; });
    printf("[replay] replaying to call %lld (%dx%d)%s\n", g_targetCall, g_width, g_height,
           g_skipUniforms ? " [uniforms skipped]" : "");
    trace::Call* call;
    while ((call = parser.parse_call())) {
        const long long no = (long long)call->no;
        if (g_callsReplayed % 100000 == 0) printf("[replay] %lld calls (at #%lld)...\n", g_callsReplayed, no);
        dispatch(call);
        g_callsReplayed++;
        const bool atTarget = no >= g_targetCall;
        delete call;
        if (atTarget) break;
    }
    printf("[replay] replayed %lld calls; ready to capture\n", g_callsReplayed);
}

// glReadPixels (JSPI-suspending) is split into its own promising call so the suspend
// happens on a short, clean wasm stack (not on top of the long replay's stack).
extern "C" EMSCRIPTEN_KEEPALIVE void replay_capture() {
    g_pixels.assign((size_t)g_width * g_height * 4, 0);
    glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
    g_captured = true;
    printf("[replay] capture done\n");
}

extern "C" EMSCRIPTEN_KEEPALIVE int replay_done() { return g_captured ? 1 : 0; }
extern "C" EMSCRIPTEN_KEEPALIVE uint8_t* replay_pixels() { return g_pixels.data(); }
extern "C" EMSCRIPTEN_KEEPALIVE int replay_width() { return g_width; }
extern "C" EMSCRIPTEN_KEEPALIVE int replay_height() { return g_height; }

// Run the (non-suspending) replay directly, then do the capture in a separate
// WebAssembly.promising call so its glReadPixels JSPI suspend sits on a clean stack.
EM_JS(void, replay_schedule, (), {
    setTimeout(function() {
        try {
            Module['_replay_run']();
            WebAssembly.promising(Module['_replay_capture'])().then(function() {
                out('[replay] capture returned');
                if (Module['onReplayDone']) Module['onReplayDone']();
            }).catch(function(e) { err('[replay] capture threw: ' + e); });
        } catch (e) { err('[replay] schedule failed: ' + e); }
    }, 200);
});

int main() {
    MobileGL::Initialize();
    static const EGLint cfgAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, nullptr, nullptr);
    EGLConfig config;
    EGLint num = 0;
    eglChooseConfig(g_dpy, cfgAttribs, &config, 1, &num);
    EGLContext ctx = eglCreateContext(g_dpy, config, EGL_NO_CONTEXT, nullptr);
    g_surf = eglCreateWindowSurface(g_dpy, config, (EGLNativeWindowType)1, nullptr);
    eglMakeCurrent(g_dpy, g_surf, g_surf, ctx);
    printf("[replay] EGL up; scheduling replay\n");
    replay_schedule();
    emscripten_set_main_loop([] {}, 0, 1); // keep runtime alive for the async replay
    return 0;
}
