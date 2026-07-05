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
#include <algorithm>
#include <array>
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
    std::unordered_map<unsigned, GLuint> g_buf, g_tex, g_fbo, g_vao, g_prog, g_shader, g_rbo, g_sampler;
    std::unordered_map<std::string, unsigned long long> g_unhandledCounts;
    std::unordered_map<std::string, unsigned long long> g_unhandledFirstCall;
    std::unordered_map<unsigned, unsigned long long> g_texInternalFormatCounts;
    std::unordered_map<unsigned, unsigned long long> g_texSwizzleParamCounts;
    std::unordered_map<unsigned, unsigned long long> g_texUploadFormatCounts;
    std::unordered_map<unsigned, unsigned long long> g_texUploadTypeCounts;
    std::unordered_map<int, unsigned long long> g_texUploadLevelCounts;
    std::unordered_map<int, unsigned long long> g_texMinFilterCounts;
    std::unordered_map<int, unsigned long long> g_texLevelParamCounts;
    std::unordered_map<unsigned, unsigned long long> g_blendFactorCounts;
    std::unordered_map<unsigned, unsigned> g_fboColor0TraceTex;
    std::unordered_map<unsigned, std::string> g_traceShaderSource;
    std::unordered_map<unsigned, GLenum> g_traceShaderType;
    std::unordered_map<unsigned, std::vector<unsigned>> g_traceProgramShaders;
    std::unordered_map<unsigned, bool> g_dumpedTraceProgramSource;
    struct TraceTexInfo {
        int width = 0;
        int height = 0;
        unsigned internalFormat = 0;
        unsigned format = 0;
        unsigned type = 0;
        int minFilter = 0;
        int magFilter = 0;
        int wrapS = 0;
        int wrapT = 0;
    };
    std::unordered_map<unsigned, TraceTexInfo> g_traceTexInfo;
    // (traceProgram<<32 | traceLocation) -> replay uniform location.
    std::unordered_map<unsigned long long, GLint> g_uniform;
    // (traceProgram<<32 | traceLocation) -> original uniform name / latest glUniform1i value.
    std::unordered_map<unsigned long long, std::string> g_uniformNameByTraceLoc;
    std::unordered_map<unsigned long long, int> g_uniform1iByTraceLoc;
    std::unordered_map<unsigned long long, std::string> g_uniformValueByTraceLoc;
    bool g_blendEnabled = false;
    GLenum g_blendSrcRgb = GL_ONE, g_blendDstRgb = GL_ZERO, g_blendSrcAlpha = GL_ONE, g_blendDstAlpha = GL_ZERO;
    GLenum g_blendEqRgb = GL_FUNC_ADD, g_blendEqAlpha = GL_FUNC_ADD;
    // Active glMapBufferRange mappings: the trace's returned pointer range -> the real
    // pointer MobileGL gave us. apitrace records writes to mapped memory as memcpy()
    // pseudo-calls whose dest is a trace pointer; we translate it into this real pointer
    // (Sodium streams all chunk vertex data through one persistent-mapped buffer).
    struct MapRange { unsigned long long traceBase; uintptr_t realPtr; unsigned long long length; };
    std::vector<MapRange> g_mappings;
    unsigned g_curTraceProg = 0; // active program (trace id), for uniform-location remap
    unsigned g_curActiveTexUnit = 0;
    unsigned g_curTraceVao = 0;
    unsigned g_boundDrawFbo = 0;
    unsigned g_boundReadFbo = 0;
    std::array<unsigned, 32> g_boundTraceTex2D{};
    std::array<unsigned, 32> g_boundTraceSampler{};
    struct TraceAttribInfo {
        bool enabled = false;
        int size = 0;
        unsigned type = 0;
        bool normalized = false;
        int stride = 0;
        unsigned long long offset = 0;
    };
    std::unordered_map<unsigned, std::array<TraceAttribInfo, 16>> g_traceVaoAttribs;
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

    // Texture uploads may source from a bound pixel-unpack buffer, in which case apitrace
    // records the final argument as a byte offset rather than an inline blob.
    const void* blobOrOffsetPtr(trace::Value& v, size_t& outSize) {
        if (const void* p = blobPtr(v, outSize)) {
            return p;
        }
        outSize = 0;
        return (const void*)(uintptr_t)v.toUIntPtr();
    }

} // namespace

static void printTailDrawState(const char* kind, long long callNo, GLenum mode, GLsizei count, GLsizei draws);
static void dumpTraceProgramSources(unsigned traceProgram);

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
    if (!strcmp(n, "glGenSamplers")) { genNames(g_sampler, c, 0, 1, glGenSamplers); return; }
    if (!strcmp(n, "glBindBuffer")) { glBindBuffer(AE(0), remap(g_buf, AU(1))); return; }
    if (!strcmp(n, "glBindTexture")) {
        if (AE(0) == GL_TEXTURE_2D && g_curActiveTexUnit < g_boundTraceTex2D.size()) {
            g_boundTraceTex2D[g_curActiveTexUnit] = AU(1);
        }
        glBindTexture(AE(0), remap(g_tex, AU(1)));
        return;
    }
    if (!strcmp(n, "glBindVertexArray")) {
        g_curTraceVao = AU(0);
        glBindVertexArray(remap(g_vao, AU(0)));
        return;
    }
    if (!strcmp(n, "glBindFramebuffer") || !strcmp(n, "glBindFramebufferEXT")) {
        if (AE(0) == GL_FRAMEBUFFER || AE(0) == GL_DRAW_FRAMEBUFFER) {
            g_boundDrawFbo = AU(1);
        }
        if (AE(0) == GL_FRAMEBUFFER || AE(0) == GL_READ_FRAMEBUFFER) {
            g_boundReadFbo = AU(1);
        }
        glBindFramebuffer(AE(0), remap(g_fbo, AU(1)));
        return;
    }
    if (!strcmp(n, "glBindSampler")) {
        if (AU(0) < g_boundTraceSampler.size()) {
            g_boundTraceSampler[AU(0)] = AU(1);
        }
        glBindSampler(AU(0), remap(g_sampler, AU(1)));
        return;
    }
    if (!strcmp(n, "glCreateProgram")) {
        if (c->ret) g_prog[(unsigned)c->ret->toUInt()] = glCreateProgram();
        return;
    }
    if (!strcmp(n, "glCreateShader")) {
        if (c->ret) {
            const unsigned traceShader = (unsigned)c->ret->toUInt();
            g_shader[traceShader] = glCreateShader(AE(0));
            g_traceShaderType[traceShader] = AE(0);
        }
        return;
    }
    if (!strcmp(n, "glAttachShader")) {
        g_traceProgramShaders[AU(0)].push_back(AU(1));
        glAttachShader(remap(g_prog, AU(0)), remap(g_shader, AU(1)));
        return;
    }
    if (!strcmp(n, "glLinkProgram")) {
        std::printf("[replay] link traceProg=%u replayProg=%u call=%llu\n", AU(0), remap(g_prog, AU(0)),
                    (unsigned long long)c->no);
        glLinkProgram(remap(g_prog, AU(0)));
        return;
    }
    if (!strcmp(n, "glUseProgram")) {
        g_curTraceProg = AU(0);
        dumpTraceProgramSources(g_curTraceProg);
        glUseProgram(remap(g_prog, AU(0)));
        return;
    }
    if (!strcmp(n, "glCompileShader")) { glCompileShader(remap(g_shader, AU(0))); return; }
    if (!strcmp(n, "glShaderSource")) {
        const GLuint sh = remap(g_shader, AU(0));
        const trace::Array* a = c->arg(2).toArray(); // array of source strings
        if (!a) return;
        std::vector<const char*> strs(a->size());
        std::string joined;
        for (size_t i = 0; i < a->size(); ++i) {
            strs[i] = a->values[i]->toString();
            joined += strs[i];
        }
        g_traceShaderSource[AU(0)] = joined;
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
            g_uniformNameByTraceLoc[((unsigned long long)tp << 32) | (unsigned)traceLoc] = c->arg(1).toString();
        }
        return;
    }
    if (!strcmp(n, "glDeleteBuffers") || !strcmp(n, "glDeleteTextures") ||
        !strcmp(n, "glDeleteFramebuffers")) {
        return; // deletes: leave resources (harmless for a one-shot replay)
    }
    if (!strcmp(n, "glDeleteProgram") || !strcmp(n, "glDeleteShader") || !strcmp(n, "glDeleteSamplers")) return;

    // --- uniforms (location remapped against the active program) ---
    auto loc = [&]() -> GLint {
        const GLint traceLoc = AS(0);
        if (traceLoc < 0) return -1;
        auto it = g_uniform.find(((unsigned long long)g_curTraceProg << 32) | (unsigned)traceLoc);
        return it != g_uniform.end() ? it->second : -1;
    };
    auto traceLocKey = [&]() -> unsigned long long {
        const GLint traceLoc = AS(0);
        return traceLoc < 0 ? 0ull : (((unsigned long long)g_curTraceProg << 32) | (unsigned)traceLoc);
    };
    if (!strcmp(n, "glUniform1i")) {
        const GLint mappedLoc = loc();
        const GLint traceLoc = AS(0);
        if (traceLoc >= 0) {
            g_uniform1iByTraceLoc[((unsigned long long)g_curTraceProg << 32) | (unsigned)traceLoc] = AS(1);
        }
        glUniform1i(mappedLoc, AS(1));
        return;
    }
    if (!strcmp(n, "glUniform1f")) { g_uniformValueByTraceLoc[traceLocKey()] = std::to_string(AF(1)); glUniform1f(loc(), AF(1)); return; }
    if (!strcmp(n, "glUniform2f")) { g_uniformValueByTraceLoc[traceLocKey()] = std::to_string(AF(1)) + "," + std::to_string(AF(2)); glUniform2f(loc(), AF(1), AF(2)); return; }
    if (!strcmp(n, "glUniform3f")) { g_uniformValueByTraceLoc[traceLocKey()] = std::to_string(AF(1)) + "," + std::to_string(AF(2)) + "," + std::to_string(AF(3)); glUniform3f(loc(), AF(1), AF(2), AF(3)); return; }
    if (!strcmp(n, "glUniform4f")) { g_uniformValueByTraceLoc[traceLocKey()] = std::to_string(AF(1)) + "," + std::to_string(AF(2)) + "," + std::to_string(AF(3)) + "," + std::to_string(AF(4)); glUniform4f(loc(), AF(1), AF(2), AF(3), AF(4)); return; }
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
        const int comps = n[9] - '0';
        if (cnt > 0 && comps >= 1 && comps <= 4) {
            std::string value;
            for (int i = 0; i < comps; ++i) {
                if (i) value += ",";
                value += std::to_string(p[i]);
            }
            g_uniformValueByTraceLoc[traceLocKey()] = value;
        }
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
    if (!strcmp(n, "glBufferSubData")) {
        size_t sz = 0; const void* d = blobPtr(c->arg(3), sz);
        glBufferSubData(AE(0), (GLintptr)c->arg(1).toUInt(), (GLsizeiptr)c->arg(2).toUInt(), d);
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
    // Persistent-mapped buffer streaming (Sodium's chunk vertex arena). Map once, write
    // via memcpy() pseudo-calls, flush ranges. Track trace-ptr -> real-ptr so memcpy
    // dests resolve to the buffer MobileGL actually mapped.
    if (!strcmp(n, "glMapBufferRange")) {
        const GLsizeiptr length = (GLsizeiptr)c->arg(2).toUInt();
        void* real = glMapBufferRange(AE(0), (GLintptr)c->arg(1).toUInt(), length,
                                      (GLbitfield)c->arg(3).toUInt());
        const unsigned long long traceRet = c->ret ? c->ret->toUIntPtr() : 0;
        if (real && traceRet) {
            g_mappings.push_back({traceRet, (uintptr_t)real, (unsigned long long)length});
        }
        return;
    }
    if (!strcmp(n, "memcpy")) {
        // ?nomap=1 disables replaying mapped-memory writes (corruption bisection aid).
        static const bool noMap = EM_ASM_INT({ return Module['noMap'] ? 1 : 0; }) != 0;
        const unsigned long long dest = c->arg(0).toUIntPtr();
        size_t sz = 0; const void* src = blobPtr(c->arg(1), sz);
        if (src && !noMap) {
            // Newest-first: the traced app can remap a buffer at the same trace
            // address (map -> unmap -> map). The oldest entry would then resolve to a
            // stale (freed/reallocated) real pointer and corrupt the heap.
            for (auto it = g_mappings.rbegin(); it != g_mappings.rend(); ++it) {
                if (dest >= it->traceBase && dest + sz <= it->traceBase + it->length) {
                    std::memcpy((void*)(it->realPtr + (dest - it->traceBase)), src, sz);
                    break;
                }
            }
        }
        return;
    }
    if (!strcmp(n, "glFlushMappedBufferRange")) {
        glFlushMappedBufferRange(AE(0), (GLintptr)c->arg(1).toUInt(), (GLsizeiptr)c->arg(2).toUInt());
        return;
    }
    if (!strcmp(n, "glUnmapBuffer")) {
        // Note: don't drop tracked mappings here. Sodium keeps one big PERSISTENT vertex
        // buffer mapped for the whole frame and only unmaps a transient (e.g. index)
        // buffer; clearing all mappings would orphan the persistent one's later writes.
        glUnmapBuffer(AE(0));
        return;
    }
    if (!strcmp(n, "glEnableVertexAttribArray")) {
        if (AU(0) < g_traceVaoAttribs[g_curTraceVao].size()) g_traceVaoAttribs[g_curTraceVao][AU(0)].enabled = true;
        glEnableVertexAttribArray(AU(0));
        return;
    }
    if (!strcmp(n, "glVertexAttribPointer")) {
        if (AU(0) < g_traceVaoAttribs[g_curTraceVao].size()) {
            auto& a = g_traceVaoAttribs[g_curTraceVao][AU(0)];
            a.size = AS(1);
            a.type = AE(2);
            a.normalized = c->arg(3).toBool();
            a.stride = AS(4);
            a.offset = c->arg(5).toUIntPtr();
        }
        glVertexAttribPointer(AU(0), AS(1), AE(2), (GLboolean)c->arg(3).toBool(), AS(4),
                              (const void*)(uintptr_t)c->arg(5).toUIntPtr());
        return;
    }
    if (!strcmp(n, "glVertexAttribIPointer")) {
        if (AU(0) < g_traceVaoAttribs[g_curTraceVao].size()) {
            auto& a = g_traceVaoAttribs[g_curTraceVao][AU(0)];
            a.size = AS(1);
            a.type = AE(2);
            a.normalized = false;
            a.stride = AS(3);
            a.offset = c->arg(4).toUIntPtr();
        }
        glVertexAttribIPointer(AU(0), AS(1), AE(2), AS(3), (const void*)(uintptr_t)c->arg(4).toUIntPtr());
        return;
    }

    // --- textures ---
    if (!strcmp(n, "glActiveTexture")) {
        const GLenum texUnit = AE(0);
        g_curActiveTexUnit = texUnit >= GL_TEXTURE0 ? static_cast<unsigned>(texUnit - GL_TEXTURE0) : 0u;
        glActiveTexture(texUnit);
        return;
    }
    if (!strcmp(n, "glTexParameteri")) {
        const unsigned traceTex = g_curActiveTexUnit < g_boundTraceTex2D.size() ? g_boundTraceTex2D[g_curActiveTexUnit] : 0u;
        if (traceTex != 0) {
            auto& info = g_traceTexInfo[traceTex];
            if (AE(1) == GL_TEXTURE_MIN_FILTER) info.minFilter = AS(2);
            else if (AE(1) == GL_TEXTURE_MAG_FILTER) info.magFilter = AS(2);
            else if (AE(1) == GL_TEXTURE_WRAP_S) info.wrapS = AS(2);
            else if (AE(1) == GL_TEXTURE_WRAP_T) info.wrapT = AS(2);
        }
        if (AE(1) == GL_TEXTURE_MIN_FILTER) {
            g_texMinFilterCounts[AS(2)]++;
        } else if (AE(1) == GL_TEXTURE_BASE_LEVEL || AE(1) == GL_TEXTURE_MAX_LEVEL) {
            g_texLevelParamCounts[AE(1)]++;
        }
        glTexParameteri(AE(0), AE(1), AS(2));
        return;
    }
    if (!strcmp(n, "glTexParameterf")) { glTexParameterf(AE(0), AE(1), AF(2)); return; }
    if (!strcmp(n, "glTexParameteriv")) {
        if (AE(1) == GL_TEXTURE_SWIZZLE_R || AE(1) == GL_TEXTURE_SWIZZLE_G || AE(1) == GL_TEXTURE_SWIZZLE_B ||
            AE(1) == GL_TEXTURE_SWIZZLE_A || AE(1) == GL_TEXTURE_SWIZZLE_RGBA) {
            g_texSwizzleParamCounts[AE(1)]++;
        }
        std::vector<int> s; const int* p = intPtr(c->arg(2), s);
        if (p) glTexParameteriv(AE(0), AE(1), p);
        return;
    }
    if (!strcmp(n, "glTexParameterfv")) {
        std::vector<float> s; const float* p = floatPtr(c->arg(2), s);
        if (p) glTexParameterfv(AE(0), AE(1), p);
        return;
    }
    if (!strcmp(n, "glSamplerParameteri")) { glSamplerParameteri(remap(g_sampler, AU(0)), AE(1), AS(2)); return; }
    if (!strcmp(n, "glSamplerParameterf")) { glSamplerParameterf(remap(g_sampler, AU(0)), AE(1), AF(2)); return; }
    if (!strcmp(n, "glSamplerParameteriv")) {
        std::vector<int> s; const int* p = intPtr(c->arg(2), s);
        if (p) glSamplerParameteriv(remap(g_sampler, AU(0)), AE(1), p);
        return;
    }
    if (!strcmp(n, "glSamplerParameterfv")) {
        std::vector<float> s; const float* p = floatPtr(c->arg(2), s);
        if (p) glSamplerParameterfv(remap(g_sampler, AU(0)), AE(1), p);
        return;
    }
    if (!strcmp(n, "glPixelStorei")) { glPixelStorei(AE(0), AS(1)); return; }
    if (!strcmp(n, "glTexImage2D")) {
        size_t sz = 0; const void* d = blobOrOffsetPtr(c->arg(8), sz);
        g_texInternalFormatCounts[(unsigned)c->arg(2).toUInt()]++;
        g_texUploadFormatCounts[(unsigned)c->arg(6).toUInt()]++;
        g_texUploadTypeCounts[(unsigned)c->arg(7).toUInt()]++;
        g_texUploadLevelCounts[AS(1)]++;
        if (AE(0) == GL_TEXTURE_2D && AS(1) == 0 && g_curActiveTexUnit < g_boundTraceTex2D.size()) {
            const unsigned traceTex = g_boundTraceTex2D[g_curActiveTexUnit];
            if (traceTex != 0) {
                auto& info = g_traceTexInfo[traceTex];
                info.width = AS(3);
                info.height = AS(4);
                info.internalFormat = AU(2);
                info.format = AU(6);
                info.type = AU(7);
            }
        }
        glTexImage2D(AE(0), AS(1), AS(2), AS(3), AS(4), AS(5), AE(6), AE(7), d);
        return;
    }
    if (!strcmp(n, "glTexSubImage2D")) {
        size_t sz = 0; const void* d = blobOrOffsetPtr(c->arg(8), sz);
        g_texUploadFormatCounts[(unsigned)c->arg(6).toUInt()]++;
        g_texUploadTypeCounts[(unsigned)c->arg(7).toUInt()]++;
        g_texUploadLevelCounts[AS(1)]++;
        glTexSubImage2D(AE(0), AS(1), AS(2), AS(3), AS(4), AS(5), AE(6), AE(7), d);
        return;
    }

    // --- framebuffers ---
    if (!strcmp(n, "glFramebufferTexture2D") || !strcmp(n, "glFramebufferTexture2DEXT")) {
        if (AE(1) == GL_COLOR_ATTACHMENT0 && AU(3) != 0) {
            g_fboColor0TraceTex[g_boundDrawFbo] = AU(3);
        }
        glFramebufferTexture2D(AE(0), AE(1), AE(2), remap(g_tex, AU(3)), AS(4));
        return;
    }
    if (!strcmp(n, "glBlitFramebuffer")) {
        if ((long long)c->no + 256 >= g_targetCall) {
            std::printf("[replay] tail glBlitFramebuffer call=%llu readFbo=%u drawFbo=%u src=(%d,%d)-(%d,%d) dst=(%d,%d)-(%d,%d) mask=0x%x filter=0x%x\n",
                        (unsigned long long)c->no, g_boundReadFbo, g_boundDrawFbo, AS(0), AS(1), AS(2), AS(3),
                        AS(4), AS(5), AS(6), AS(7), (unsigned)c->arg(8).toUInt(), AE(9));
        }
        glBlitFramebuffer(AS(0), AS(1), AS(2), AS(3), AS(4), AS(5), AS(6), AS(7),
                          (GLbitfield)c->arg(8).toUInt(), AE(9));
        return;
    }
    if (!strcmp(n, "glCheckFramebufferStatus") || !strcmp(n, "glCheckFramebufferStatusEXT")) {
        return; // query, no side effect
    }
    // EXT_framebuffer_object aliases (older traces, e.g. OpenRA) route to the core
    // entry points. Renderbuffers back such FBOs' depth attachments.
    if (!strcmp(n, "glGenFramebuffersEXT")) { genNames(g_fbo, c, 0, 1, glGenFramebuffers); return; }
    if (!strcmp(n, "glGenRenderbuffers") || !strcmp(n, "glGenRenderbuffersEXT")) {
        genNames(g_rbo, c, 0, 1, glGenRenderbuffers);
        return;
    }
    if (!strcmp(n, "glBindRenderbuffer") || !strcmp(n, "glBindRenderbufferEXT")) {
        glBindRenderbuffer(AE(0), remap(g_rbo, AU(1)));
        return;
    }
    if (!strcmp(n, "glRenderbufferStorage") || !strcmp(n, "glRenderbufferStorageEXT")) {
        glRenderbufferStorage(AE(0), AE(1), AS(2), AS(3));
        return;
    }
    if (!strcmp(n, "glFramebufferRenderbuffer") || !strcmp(n, "glFramebufferRenderbufferEXT")) {
        glFramebufferRenderbuffer(AE(0), AE(1), AE(2), remap(g_rbo, AU(3)));
        return;
    }

    // --- render state ---
    if (!strcmp(n, "glEnable")) {
        if (AE(0) == GL_FRAMEBUFFER_SRGB) {
            std::printf("[replay] glEnable(GL_FRAMEBUFFER_SRGB) call=%llu\n", (unsigned long long)c->no);
        }
        if (AE(0) == GL_BLEND) g_blendEnabled = true;
        glEnable(AE(0));
        return;
    }
    if (!strcmp(n, "glDisable")) {
        if (AE(0) == GL_FRAMEBUFFER_SRGB) {
            std::printf("[replay] glDisable(GL_FRAMEBUFFER_SRGB) call=%llu\n", (unsigned long long)c->no);
        }
        if (AE(0) == GL_BLEND) g_blendEnabled = false;
        glDisable(AE(0));
        return;
    }
    if (!strcmp(n, "glCullFace")) { glCullFace(AE(0)); return; }
    if (!strcmp(n, "glFrontFace")) { glFrontFace(AE(0)); return; }
    if (!strcmp(n, "glDepthFunc")) { glDepthFunc(AE(0)); return; }
    if (!strcmp(n, "glDepthMask")) { glDepthMask((GLboolean)c->arg(0).toBool()); return; }
    if (!strcmp(n, "glDepthRange")) { glDepthRange(c->arg(0).toDouble(), c->arg(1).toDouble()); return; }
    if (!strcmp(n, "glDepthRangef")) { glDepthRange((GLdouble)AF(0), (GLdouble)AF(1)); return; }
    if (!strcmp(n, "glPolygonOffset")) { glPolygonOffset(AF(0), AF(1)); return; }
    if (!strcmp(n, "glBlendFunc")) {
        g_blendFactorCounts[AE(0)]++;
        g_blendFactorCounts[AE(1)]++;
        g_blendSrcRgb = AE(0);
        g_blendDstRgb = AE(1);
        g_blendSrcAlpha = AE(0);
        g_blendDstAlpha = AE(1);
        glBlendFunc(AE(0), AE(1));
        return;
    }
    if (!strcmp(n, "glBlendFuncSeparate")) {
        g_blendFactorCounts[AE(0)]++;
        g_blendFactorCounts[AE(1)]++;
        g_blendFactorCounts[AE(2)]++;
        g_blendFactorCounts[AE(3)]++;
        g_blendSrcRgb = AE(0);
        g_blendDstRgb = AE(1);
        g_blendSrcAlpha = AE(2);
        g_blendDstAlpha = AE(3);
        glBlendFuncSeparate(AE(0), AE(1), AE(2), AE(3));
        return;
    }
    if (!strcmp(n, "glBlendEquation")) {
        g_blendEqRgb = AE(0);
        g_blendEqAlpha = AE(0);
        glBlendEquation(AE(0));
        return;
    }
    if (!strcmp(n, "glBlendEquationSeparate")) {
        g_blendEqRgb = AE(0);
        g_blendEqAlpha = AE(1);
        glBlendEquationSeparate(AE(0), AE(1));
        return;
    }
    if (!strcmp(n, "glBlendColor")) { glBlendColor(AF(0), AF(1), AF(2), AF(3)); return; }
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
    if (!strcmp(n, "glDrawArrays")) {
        printTailDrawState("glDrawArrays", static_cast<long long>(c->no), AE(0), AS(2), 1);
        glDrawArrays(AE(0), AS(1), AS(2));
        return;
    }
    if (!strcmp(n, "glDrawElements")) {
        printTailDrawState("glDrawElements", static_cast<long long>(c->no), AE(0), AS(1), 1);
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
        printTailDrawState("glMultiDrawElementsBaseVertex", static_cast<long long>(c->no), AE(0), counts[0], dc);
        glMultiDrawElementsBaseVertex(AE(0), counts.data(), AE(2), idx.data(), dc, bv.data());
        return;
    }

    // Everything else (queries, sync, mapping, debug, swapbuffers) is skipped: it has no
    // bearing on the captured frame for this fixture.
    const std::string name = n ? n : "<null>";
    g_unhandledCounts[name]++;
    if (!g_unhandledFirstCall.count(name)) {
        g_unhandledFirstCall[name] = static_cast<unsigned long long>(c->no);
    }
}

static void printUnhandledSummary() {
    std::vector<std::pair<std::string, unsigned long long>> entries(g_unhandledCounts.begin(), g_unhandledCounts.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] unhandled call summary (%zu kinds)\n", entries.size());
    for (const auto& [name, count] : entries) {
        const auto it = g_unhandledFirstCall.find(name);
        const unsigned long long first = (it != g_unhandledFirstCall.end()) ? it->second : 0ull;
        std::printf("[replay]   %s count=%llu first=%llu\n", name.c_str(), count, first);
    }
}

static void printTextureUsageSummary() {
    std::vector<std::pair<unsigned, unsigned long long>> formats(g_texInternalFormatCounts.begin(),
                                                                 g_texInternalFormatCounts.end());
    std::sort(formats.begin(), formats.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture internal formats (%zu kinds)\n", formats.size());
    for (const auto& [fmt, count] : formats) {
        std::printf("[replay]   internalFormat=0x%x count=%llu\n", fmt, count);
    }
    std::vector<std::pair<unsigned, unsigned long long>> swizzles(g_texSwizzleParamCounts.begin(),
                                                                  g_texSwizzleParamCounts.end());
    std::sort(swizzles.begin(), swizzles.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture swizzle params (%zu kinds)\n", swizzles.size());
    for (const auto& [pname, count] : swizzles) {
        std::printf("[replay]   pname=0x%x count=%llu\n", pname, count);
    }
    std::vector<std::pair<unsigned, unsigned long long>> uploadFormats(g_texUploadFormatCounts.begin(),
                                                                       g_texUploadFormatCounts.end());
    std::sort(uploadFormats.begin(), uploadFormats.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture upload formats (%zu kinds)\n", uploadFormats.size());
    for (const auto& [fmt, count] : uploadFormats) {
        std::printf("[replay]   format=0x%x count=%llu\n", fmt, count);
    }
    std::vector<std::pair<unsigned, unsigned long long>> uploadTypes(g_texUploadTypeCounts.begin(),
                                                                     g_texUploadTypeCounts.end());
    std::sort(uploadTypes.begin(), uploadTypes.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture upload types (%zu kinds)\n", uploadTypes.size());
    for (const auto& [type, count] : uploadTypes) {
        std::printf("[replay]   type=0x%x count=%llu\n", type, count);
    }
    std::vector<std::pair<int, unsigned long long>> levels(g_texUploadLevelCounts.begin(), g_texUploadLevelCounts.end());
    std::sort(levels.begin(), levels.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture upload levels (%zu kinds)\n", levels.size());
    for (const auto& [level, count] : levels) {
        std::printf("[replay]   level=%d count=%llu\n", level, count);
    }
    std::vector<std::pair<int, unsigned long long>> minFilters(g_texMinFilterCounts.begin(), g_texMinFilterCounts.end());
    std::sort(minFilters.begin(), minFilters.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture min filters (%zu kinds)\n", minFilters.size());
    for (const auto& [filter, count] : minFilters) {
        std::printf("[replay]   minFilter=0x%x count=%llu\n", filter, count);
    }
    std::vector<std::pair<int, unsigned long long>> levelParams(g_texLevelParamCounts.begin(), g_texLevelParamCounts.end());
    std::sort(levelParams.begin(), levelParams.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] texture level params (%zu kinds)\n", levelParams.size());
    for (const auto& [pname, count] : levelParams) {
        std::printf("[replay]   pname=0x%x count=%llu\n", pname, count);
    }
    std::vector<std::pair<unsigned, unsigned long long>> blendFactors(g_blendFactorCounts.begin(), g_blendFactorCounts.end());
    std::sort(blendFactors.begin(), blendFactors.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::printf("[replay] blend factors (%zu kinds)\n", blendFactors.size());
    for (const auto& [factor, count] : blendFactors) {
        std::printf("[replay]   factor=0x%x count=%llu\n", factor, count);
    }
}

static void dumpTraceProgramSources(unsigned traceProgram) {
    if (g_dumpedTraceProgramSource[traceProgram]) {
        return;
    }
    g_dumpedTraceProgramSource[traceProgram] = true;
    // ?dumpprog=N in the shell URL dumps trace program N's GLSL on first use.
    static const unsigned dumpProg =
        (unsigned)EM_ASM_INT({ return Module['dumpProg'] || 0; });
    if (traceProgram != dumpProg && traceProgram != 64 && traceProgram != 111) {
        return;
    }
    std::printf("[replay] trace program %u shader dump begin\n", traceProgram);
    const auto it = g_traceProgramShaders.find(traceProgram);
    if (it == g_traceProgramShaders.end()) {
        std::printf("[replay] trace program %u has no recorded shaders\n", traceProgram);
        return;
    }
    for (unsigned traceShader : it->second) {
        const GLenum stage = g_traceShaderType.count(traceShader) ? g_traceShaderType[traceShader] : 0u;
        const auto srcIt = g_traceShaderSource.find(traceShader);
        std::printf("[replay] shader %u stage=0x%x\n", traceShader, static_cast<unsigned>(stage));
        if (srcIt != g_traceShaderSource.end()) {
            std::printf("%s\n", srcIt->second.c_str());
        }
    }
    std::printf("[replay] trace program %u shader dump end\n", traceProgram);
}

static void printTailDrawState(const char* kind, long long callNo, GLenum mode, GLsizei count, GLsizei draws) {
    if (callNo + 256 < g_targetCall) {
        return;
    }
    const unsigned fboColor0 = g_fboColor0TraceTex.count(g_boundDrawFbo) ? g_fboColor0TraceTex[g_boundDrawFbo] : 0u;
    std::printf("[replay] tail %s call=%lld traceProg=%u fbo=%u color0=%u mode=0x%x count=%d draws=%d blend=%d src=(0x%x,0x%x) dst=(0x%x,0x%x) eq=(0x%x,0x%x)\n",
                kind, callNo, g_curTraceProg, g_boundDrawFbo, fboColor0, static_cast<unsigned>(mode), count, draws,
                g_blendEnabled ? 1 : 0, static_cast<unsigned>(g_blendSrcRgb), static_cast<unsigned>(g_blendSrcAlpha),
                static_cast<unsigned>(g_blendDstRgb), static_cast<unsigned>(g_blendDstAlpha),
                static_cast<unsigned>(g_blendEqRgb), static_cast<unsigned>(g_blendEqAlpha));
    const auto vaoIt = g_traceVaoAttribs.find(g_curTraceVao);
    const std::array<TraceAttribInfo, 16> emptyAttribs{};
    const auto& attribs = vaoIt != g_traceVaoAttribs.end() ? vaoIt->second : emptyAttribs;
    for (int i = 0; i < 3; ++i) {
        const auto& a = attribs[(size_t)i];
        std::printf("[replay]   attrib%d enabled=%d size=%d type=0x%x norm=%d stride=%d offset=%llu\n", i,
                    a.enabled ? 1 : 0, a.size, a.type, a.normalized ? 1 : 0, a.stride, a.offset);
    }
    for (const auto& [key, name] : g_uniformNameByTraceLoc) {
        if ((key >> 32) != static_cast<unsigned long long>(g_curTraceProg)) {
            continue;
        }
        const auto it = g_uniform1iByTraceLoc.find(key);
        if (it != g_uniform1iByTraceLoc.end()) {
            const int unit = it->second;
            if (unit < 0 || unit >= static_cast<int>(g_boundTraceTex2D.size())) {
                std::printf("[replay]   uniform name=%s unit=%d tex=<out-of-range> samp=<out-of-range>\n",
                            name.c_str(), unit);
                continue;
            }
            const unsigned traceTex = g_boundTraceTex2D[static_cast<size_t>(unit)];
            const auto texIt = g_traceTexInfo.find(traceTex);
            GLint actualMin = 0, actualMag = 0, actualWrapS = 0, actualWrapT = 0;
            const unsigned savedActive = g_curActiveTexUnit;
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &actualMin);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &actualMag);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &actualWrapS);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &actualWrapT);
            glActiveTexture(GL_TEXTURE0 + savedActive);
            if (texIt != g_traceTexInfo.end()) {
                const auto& info = texIt->second;
                std::printf("[replay]   uniform name=%s unit=%d tex=%u %dx%d ifmt=0x%x fmt=0x%x type=0x%x min=0x%x/%x mag=0x%x/%x wrap=(0x%x/%x,0x%x/%x) samp=%u\n",
                            name.c_str(), unit, traceTex, info.width, info.height, info.internalFormat, info.format,
                            info.type, info.minFilter, actualMin, info.magFilter, actualMag, info.wrapS, actualWrapS,
                            info.wrapT, actualWrapT,
                            g_boundTraceSampler[static_cast<size_t>(unit)]);
            } else {
                std::printf("[replay]   uniform name=%s unit=%d tex=%u min=0x%x mag=0x%x wrap=(0x%x,0x%x) samp=%u\n",
                            name.c_str(), unit, traceTex, actualMin, actualMag, actualWrapS, actualWrapT,
                            g_boundTraceSampler[static_cast<size_t>(unit)]);
            }
        } else {
            const auto valueIt = g_uniformValueByTraceLoc.find(key);
            if (valueIt != g_uniformValueByTraceLoc.end()) {
                std::printf("[replay]   uniform name=%s value=%s\n", name.c_str(), valueIt->second.c_str());
            }
        }
    }
}

// Replays the trace up to the target call, then reads back the framebuffer.
extern "C" EMSCRIPTEN_KEEPALIVE void replay_run() {
    trace::Parser parser;
    if (!parser.open(kTracePath)) {
        printf("[replay] failed to open %s\n", kTracePath);
        return;
    }
    const long long overrideTargetCall = EM_ASM_INT({ return Module['targetCall'] || 0; });
    if (overrideTargetCall > 0) {
        g_targetCall = overrideTargetCall;
    }
    printf("[replay] replaying to call %lld (%dx%d)\n", g_targetCall, g_width, g_height);
    trace::Call* call;
    while ((call = parser.parse_call())) {
        const long long no = (long long)call->no;
        if (g_callsReplayed % 10000 == 0) printf("[replay] %lld calls (at #%lld)...\n", g_callsReplayed, no);
        dispatch(call);
        // Submit pending GPU commands periodically so the backend's single command
        // buffer doesn't grow unbounded over ~900K calls (which fails to submit).
        // Safe mid-frame: render targets persist and draws use loadOp=Load.
        if (g_callsReplayed % 1000 == 0) glFlush();
        g_callsReplayed++;
        const bool atTarget = no >= g_targetCall;
        delete call;
        if (atTarget) break;
    }
    printUnhandledSummary();
    printTextureUsageSummary();
    std::printf("[replay] framebuffer_srgb final=%d\n", glIsEnabled(GL_FRAMEBUFFER_SRGB) ? 1 : 0);
    printf("[replay] replayed %lld calls; ready to capture\n", g_callsReplayed);
}

// glReadPixels (JSPI-suspending) is split into its own promising call so the suspend
// happens on a short, clean wasm stack (not on top of the long replay's stack).
extern "C" EMSCRIPTEN_KEEPALIVE void replay_capture() {
    // ?capw=/?caph= override the capture size (e.g. reading a small FBO mid-frame
    // when bisecting with ?target=). The default is the full surface.
    const int capW = EM_ASM_INT({ return Module['capW'] || 0; });
    const int capH = EM_ASM_INT({ return Module['capH'] || 0; });
    if (capW > 0 && capH > 0) {
        g_width = capW;
        g_height = capH;
    }
    g_pixels.assign((size_t)g_width * g_height * 4, 0);
    std::printf("[replay] capture readFbo=%u drawFbo=%u %dx%d\n", g_boundReadFbo, g_boundDrawFbo,
                g_width, g_height);
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
    // ?w=/?h= (shell) override the surface size for fixtures not captured at 854x480
    // (the shell resizes the canvas to match before the module starts).
    const int surfW = EM_ASM_INT({ return Module['surfW'] || 0; });
    const int surfH = EM_ASM_INT({ return Module['surfH'] || 0; });
    if (surfW > 0 && surfH > 0) {
        g_width = surfW;
        g_height = surfH;
    }
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
