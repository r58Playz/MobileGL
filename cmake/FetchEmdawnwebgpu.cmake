# cmake/FetchEmdawnwebgpu.cmake
#
# Fetches the emdawnwebgpu package (Dawn's standardized-webgpu.h bindings for
# Emscripten) from the pinned Dawn GitHub release, verifies its SHA-512, and
# applies the small Emscripten-3.1.56 compatibility fix
# (3rdparty/emdawnwebgpu-3.1.56.patch). Nothing third-party is committed: only
# this module, the version pin, and the one-line patch live in the tree; the
# package itself is downloaded into the build directory at configure time.
#
# Provenance:
#   Dawn release emdawnwebgpu_pkg-${EMDAWNWEBGPU_VERSION}.zip
#   (Dawn rev b465e23d19a5e27402d80191c0f8de1fb3b385b5; built/tested vs emsdk 4.0.3)
#
# Exposes (parent scope):
#   EMDAWNWEBGPU_PKG_DIR        - extracted package root
#   EMDAWNWEBGPU_INCLUDE_DIRS   - include dirs for <webgpu/webgpu.h> (+ C++ bindings)
#   EMDAWNWEBGPU_WEBGPU_CPP     - the webgpu.cpp glue source to compile into the lib
#   EMDAWNWEBGPU_JS_LIBRARIES   - the four --js-library files for the final link (M3+)
#
# Offline / local override: set FETCHCONTENT_SOURCE_DIR_EMDAWNWEBGPU to a local
# checkout of the extracted package (containing emdawnwebgpu_pkg/).

include(FetchContent)

set(EMDAWNWEBGPU_VERSION "v20250428.134257"
    CACHE STRING "Pinned Dawn emdawnwebgpu release tag")
set(EMDAWNWEBGPU_SHA512
    "e8eb56157f52c229e87848a6c9cd062035a987ee420cfe05287dc7b7d6bd0cae1cdcd8d010ad362f40f6a785b3133ca2efe0e655c94f5b3d92210fb0e59c574e"
    CACHE STRING "SHA-512 of emdawnwebgpu_pkg-${EMDAWNWEBGPU_VERSION}.zip")

FetchContent_Declare(
    emdawnwebgpu
    URL "https://github.com/google/dawn/releases/download/${EMDAWNWEBGPU_VERSION}/emdawnwebgpu_pkg-${EMDAWNWEBGPU_VERSION}.zip"
    URL_HASH "SHA512=${EMDAWNWEBGPU_SHA512}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(emdawnwebgpu)

# FetchContent strips the archive's single top-level emdawnwebgpu_pkg/ dir, so the
# package contents land directly in the populated source dir.
set(EMDAWNWEBGPU_PKG_DIR "${emdawnwebgpu_SOURCE_DIR}")
if (NOT EXISTS "${EMDAWNWEBGPU_PKG_DIR}/webgpu/include/webgpu/webgpu.h")
    message(FATAL_ERROR "emdawnwebgpu package layout unexpected at ${EMDAWNWEBGPU_PKG_DIR}")
endif()

# Apply both 3.1.56 compatibility edits independently. A single `git apply`
# cannot recover when an interrupted/older configure applied only one hunk.
set(_emdawn_library "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/library_webgpu.js")
file(READ "${_emdawn_library}" _emdawn_js)
set(_emdawn_js_original "${_emdawn_js}")

set(_emdawn_old_deps "errorCallback__deps: ['$stackSave', '$stackRestore', '$stringToUTF8OnStack']")
set(_emdawn_new_deps "errorCallback__deps: ['stackSave', 'stackRestore', '$stringToUTF8OnStack']")
string(FIND "${_emdawn_js}" "${_emdawn_new_deps}" _emdawn_has_new_deps)
if (_emdawn_has_new_deps EQUAL -1)
    string(FIND "${_emdawn_js}" "${_emdawn_old_deps}" _emdawn_has_old_deps)
    if (_emdawn_has_old_deps EQUAL -1)
        message(FATAL_ERROR "emdawnwebgpu errorCallback dependency syntax is unrecognized")
    endif()
    string(REPLACE "${_emdawn_old_deps}" "${_emdawn_new_deps}" _emdawn_js "${_emdawn_js}")
endif()

set(_emdawn_old_wait "emwgpuWaitAny: () => {")
set(_emdawn_new_wait "emwgpuWaitAny: (futurePtr, futureCount, timeoutNSPtr) => {")
string(FIND "${_emdawn_js}" "${_emdawn_new_wait}" _emdawn_has_new_wait)
if (_emdawn_has_new_wait EQUAL -1)
    string(FIND "${_emdawn_js}" "${_emdawn_old_wait}" _emdawn_has_old_wait)
    if (_emdawn_has_old_wait EQUAL -1)
        message(FATAL_ERROR "emdawnwebgpu asyncify-free WaitAny stub syntax is unrecognized")
    endif()
    string(REPLACE "${_emdawn_old_wait}" "${_emdawn_new_wait}" _emdawn_js "${_emdawn_js}")
endif()

# >=2GB pointer fix: emdawnwebgpu keys its single jsObjects table by raw pointer, but the
# _emwgpuCreate* wasm returns are read into JS as SIGNED i32 (negative for a >=2GB pointer
# under CAN_ADDRESS_2GB, e.g. the dotnet host's ~4GB heap), so inserts land under a negative
# key while every lookup is >>>0'd (unsigned) -> getJsObject misses -> undefined handle ->
# TextureCreateView / RenderPassEncoderEnd / etc. crash. Normalize every jsObjects key to
# unsigned. Idempotent: the replacement no longer contains the "[ptr]" substring.
string(FIND "${_emdawn_js}" "WebGPU.Internals.jsObjects[ptr >>> 0]" _emdawn_has_ptrfix)
if (_emdawn_has_ptrfix EQUAL -1)
    string(FIND "${_emdawn_js}" "WebGPU.Internals.jsObjects[ptr]" _emdawn_has_rawkey)
    if (_emdawn_has_rawkey EQUAL -1)
        message(FATAL_ERROR "emdawnwebgpu jsObjects key-access syntax is unrecognized")
    endif()
    string(REPLACE "WebGPU.Internals.jsObjects[ptr]" "WebGPU.Internals.jsObjects[ptr >>> 0]" _emdawn_js "${_emdawn_js}")
endif()

if (NOT _emdawn_js STREQUAL _emdawn_js_original)
    file(WRITE "${_emdawn_library}" "${_emdawn_js}")
    message(STATUS "emdawnwebgpu: applied 3.1.56 compatibility edits")
else()
    message(STATUS "emdawnwebgpu: 3.1.56 compatibility edits already applied")
endif()

set(EMDAWNWEBGPU_INCLUDE_DIRS
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu/include"
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu_cpp/include")
set(EMDAWNWEBGPU_WEBGPU_CPP "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/webgpu.cpp")
# The JS libraries must be passed at the FINAL link (--js-library), in this order.
set(EMDAWNWEBGPU_JS_LIBRARIES
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/library_webgpu_enum_tables.js"
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/library_webgpu_generated_struct_info.js"
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/library_webgpu_generated_sig_info.js"
    "${EMDAWNWEBGPU_PKG_DIR}/webgpu/src/library_webgpu.js"
    CACHE INTERNAL "emdawnwebgpu --js-library files (ordered) for the final link")

message(STATUS "emdawnwebgpu ${EMDAWNWEBGPU_VERSION} ready at ${EMDAWNWEBGPU_PKG_DIR}")
