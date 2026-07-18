cmake_minimum_required(VERSION 3.22.1)

project("MobileGL")

option(MOBILEGL_BUILD_TEST        "Build MobileGL tests"                             ON )
option(MOBILEGL_BUILD_BENCHMARK   "Build MobileGL benchmarks"                        ON )
option(MOBILEGL_FORCE_RELEASE_OPT "Enable Release optimization flags in Debug build" ON )
option(MOBILEGL_ENABLE_TRACY      "Enable tracy for profiling"                       OFF)
option(MOBILEGL_BUILD_TRACE_REPLAY "Build desktop apitrace replay runner"            OFF)
option(MOBILEGL_IOS               "Build MobileGL for iOS instead of macOS when APPLE is set" OFF)
# Emscripten GL-translation-backend adapter for the patched pongasoft emscripten-glfw
# (exports mobilegl_webgl_* / mobilegl_get_proc_address over MobileGL's EGL/WebGPU flow).
option(MOBILEGL_EMSCRIPTEN_GLFW   "Build the emscripten-glfw GL-backend adapter"     OFF)
# Fixed-function pipeline emulation (SimpleFPEWrapper) in front of MobileGL, for legacy
# apps such as Minecraft 1.16 and lower. Requires MOBILEGL_EMSCRIPTEN_GLFW.
option(MOBILEGL_EMSCRIPTEN_GLFW_FPE "Enable SimpleFPEWrapper fixed-function front layer" OFF)
set(MOBILEGL_LOG_ACTIVE_LEVEL "MOBILEGL_LOG_LEVEL_INFO" CACHE STRING "MobileGL active log level macro")
set(MOBILEGL_VULKAN_LIBRARY "" CACHE FILEPATH "Vulkan loader/MoltenVK library to link for iOS builds")

if (ANDROID)
    set(MOBILEGL_BUILD_TEST       OFF CACHE BOOL "Build MobileGL tests"      FORCE)
    set(MOBILEGL_BUILD_BENCHMARK  OFF CACHE BOOL "Build MobileGL benchmarks" FORCE)
endif()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR MOBILEGL_FORCE_RELEASE_OPT)
    # Check if ThinLTO or LTO is suppported
    include(CheckIPOSupported)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)

    check_ipo_supported(RESULT LTOSupported OUTPUT LTOError)

    check_c_compiler_flag("-flto" HAS_LTO_C)
    check_cxx_compiler_flag("-flto" HAS_LTO_CXX)

    if (LTOSupported OR (HAS_LTO_C AND HAS_LTO_CXX))
        # Check ThinLTO
        check_c_compiler_flag("-flto=thin" HAS_THINLTO_C)
        check_cxx_compiler_flag("-flto=thin" HAS_THINLTO_CXX)
        if (HAS_THINLTO_C AND HAS_THINLTO_CXX)
            message(STATUS "ThinLTO supported, using -flto=thin")

            add_compile_options(-flto=thin)
            add_link_options(-flto=thin)
        else()
            # ThinLTO is not supported
            message(STATUS "ThinLTO not available, fallback to CMAKE IPO")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    else()
        message(STATUS "IPO not supported: ${LTOError}")
    endif()

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MATCHES "AppleClang")
        add_compile_options(-O3 -ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
#        add_compile_options(/O2)
    else ()
        add_compile_options(-O2)
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/Zc:__cplusplus)
endif()

enable_language(CXX)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -fPIC. Turn OFF when static-linking MobileGL into a non-PIC (non-MAIN_MODULE) Emscripten binary:
# under -fPIC the default-visibility exported gl*/egl* symbols' addresses become GOT.func entries
# that resolve to null in such a link, so proc-address lookups hand LWJGL null function pointers.
option(MOBILEGL_POSITION_INDEPENDENT_CODE "Build MobileGL with -fPIC" ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ${MOBILEGL_POSITION_INDEPENDENT_CODE})

if (MSVC)
    set(CMAKE_CXX_FLAGS "/EHsc ${CMAKE_CXX_FLAGS}")
endif()

# Generate MGGitHash.h
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH_FULL
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

string(SUBSTRING "${GIT_COMMIT_HASH_FULL}" 0 7 GIT_COMMIT_HASH_SHORT)

configure_file(
    ${CMAKE_SOURCE_DIR}/MobileGL/MG_Util/Miscellany/MGGitHash.h.in
    ${CMAKE_BINARY_DIR}/generated/MGGitHash.h
    @ONLY
)

include_directories(${CMAKE_BINARY_DIR}/generated)

set(ENABLE_RTTI                ON  CACHE BOOL "Enables RTTI (will be took by glslang)" FORCE)

set(ENABLE_GLSLANG_BINARIES    OFF CACHE BOOL "Enable glslangValidator/spirv-remap" FORCE)
set(ENABLE_SPVREMAPPER         OFF CACHE BOOL "Enable SPVRemapper" FORCE)
set(ENABLE_OPT                 ON  CACHE BOOL "Enable SPIRV-Tools opt usage in glslang" FORCE)
set(BUILD_EXTERNAL             ON  CACHE BOOL "Build external deps in External/" FORCE)
set(ENABLE_GLSLANG_INSTALL     OFF CACHE BOOL "Install glslang targets" FORCE)

set(SPIRV_CROSS_C_API          ON  CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL    ON  CACHE BOOL "Enable GLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL    OFF CACHE BOOL "Disable HLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_MSL     OFF CACHE BOOL "Disable MSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_CPP     OFF CACHE BOOL "Disable C++ API target" FORCE)
set(SPIRV_CROSS_CLI            OFF CACHE BOOL "Disable CLI binary" FORCE)
set(SPIRV_CROSS_STATIC         ON  CACHE BOOL "Prefer static libs" FORCE)

set(SPIRV_REFLECT_EXECUTABLE     OFF CACHE BOOL "Build spirv-reflect executable" FORCE)
set(SPIRV_REFLECT_STATIC_LIB     ON  CACHE BOOL "Build a SPIRV-Reflect static library" FORCE)
set(SPIRV_REFLECT_BUILD_TESTS    OFF CACHE BOOL "Build the SPIRV-Reflect test suite" FORCE)
set(SPIRV_REFLECT_ENABLE_ASSERTS OFF CACHE BOOL  "Enable asserts for debugging" FORCE)
set(SPIRV_REFLECT_ENABLE_ASAN    OFF CACHE BOOL "Use address sanitization" FORCE)
set(SPIRV_REFLECT_INSTALL        OFF CACHE BOOL "Whether to install" FORCE)

# add_subdirectory(3rdparty/DiligentCore)
add_subdirectory(3rdparty/glslang)
add_subdirectory(3rdparty/SPIRV-Cross)
if (NOT EMSCRIPTEN)
    add_subdirectory(3rdparty/VulkanMemoryAllocator)
    add_subdirectory(3rdparty/Vulkan-Headers)
    add_subdirectory(3rdparty/Vulkan-Utility-Libraries)
endif()
add_subdirectory(3rdparty/SPIRV-Reflect)

if (EMSCRIPTEN)
    # WebGPU backend: fetch + patch the vendored emdawnwebgpu package (downloaded,
    # not committed). Sets EMDAWNWEBGPU_INCLUDE_DIRS / _WEBGPU_CPP / _JS_LIBRARIES.
    include(${CMAKE_SOURCE_DIR}/cmake/FetchEmdawnwebgpu.cmake)

    # Prebuilt tint (SPIR-V -> WGSL), produced by tools/build-tint.sh (one combined
    # libtint.a + a header bundle). Browser WebGPU accepts only WGSL.
    set(MOBILEGL_TINT_DIR "${CMAKE_SOURCE_DIR}/tools/tint/out" CACHE PATH "Prebuilt tint output dir (tools/build-tint.sh)")
    if (NOT EXISTS "${MOBILEGL_TINT_DIR}/libtint.a")
        message(FATAL_ERROR "tint not built: '${MOBILEGL_TINT_DIR}/libtint.a' is missing. Run tools/build-tint.sh first (or set -DMOBILEGL_TINT_DIR).")
    endif()
    add_library(mobilegl_tint STATIC IMPORTED GLOBAL)
    set_target_properties(mobilegl_tint PROPERTIES IMPORTED_LOCATION "${MOBILEGL_TINT_DIR}/libtint.a")
endif()

set(XXHASH_BUILD_XXHSUM OFF)
option(BUILD_SHARED_LIBS OFF)
add_subdirectory(3rdparty/xxHash/build/cmake xxhash_build EXCLUDE_FROM_ALL)

set(TRACY_ENABLE ${MOBILEGL_ENABLE_TRACY} CACHE BOOL "Enable Tracy, this is an internal variable" FORCE)

if (TRACY_ENABLE)
    set(TRACY_ON_DEMAND            ON  CACHE BOOL "Enable profiling only connected" FORCE)
    set(TRACY_NO_EXIT              OFF CACHE BOOL "Don't exit Tracy until connected" FORCE)
    set(TRACY_DELAYED_INIT         ON  CACHE BOOL "Don't init Tracy on library load" FORCE)
    set(TRACY_MANUAL_LIFETIME      ON  CACHE BOOL "Manually control Tracy lifetime" FORCE)
    set(TRACY_NO_CRASH_HANDLER     ON  CACHE BOOL "Disable crash handling" FORCE)

    add_subdirectory(3rdparty/tracy)
endif ()

set(SOURCE_FILES
    MobileGL/Init.cpp
    MobileGL/GlobalObjects.cpp
    MobileGL/ConfigLoader.cpp

    MobileGL/MG_Util/Debug/Log.cpp

    MobileGL/MG_Util/Math/VectorTypes.cpp
    MobileGL/MG_Util/Metrics/TextureMetrics.cpp

    MobileGL/MG_Util/Metrics/BufferMetrics.cpp

    MobileGL/MG_Util/Converters/GLToStr/GLEnumConverter.cpp
    MobileGL/MG_Util/Converters/EGLToStr/EGLEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/GLExtensionConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToGlslang/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/ErrorCodeConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToMG/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToVk/TextureEnumConverter.cpp

    MobileGL/MG_Util/Classifiers/TextureEnumClassifier.cpp

    MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
    MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.cpp
    MobileGL/MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenInterfaceStructPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EliminateFloatEqualsZeroPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameSamplerFunctionParameterPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecomposeWorkgroupVec3Pass.cpp

    MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
    MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp

    MobileGL/MG_Util/Texture/PixelStoreProcessor.cpp
    MobileGL/MG_Util/Texture/TextureFormatProcessor.cpp

    MobileGL/MG_Impl/GLXImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/GLXImpl/LookUp/LookUp.cpp

    MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp
    
    # @INSERTION_POINT:SOURCE_FILE_GLIMPL@ #
    MobileGL/MG_Impl/GLImpl/Sampler/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Sampler/GL_Sampler.cpp
    MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.cpp
    MobileGL/MG_Impl/GLImpl/RenderState/GL_RenderState.cpp
    MobileGL/MG_Impl/GLImpl/Framebuffer/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp
    MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
    MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp
    MobileGL/MG_Impl/GLImpl/Texture/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Texture/ProxyTexture.cpp
    MobileGL/MG_Impl/GLImpl/VertexArray/GL_VertexArray.cpp
    MobileGL/MG_Impl/GLImpl/VertexArray/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.cpp
    MobileGL/MG_Impl/GLImpl/Buffer/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp
    MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.cpp

    MobileGL/MG_Impl/Init.cpp
    MobileGL/MG_Impl/GetProcAddress.cpp

    MobileGL/MG_Backend/Init.cpp
    MobileGL/MG_Backend/BackendObject.cpp

    MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
    MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
    MobileGL/MG_Backend/DirectGLES/Utils.cpp
    MobileGL/MG_Backend/DirectGLES/Managers.cpp

    MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
    MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp
    MobileGL/MG_Backend/DirectVulkan/VmaImpl.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/FrameContext.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/BufferArena.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateBuilder.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp

    MobileGL/MG_State/GLState/Core.cpp
    MobileGL/MG_State/EGLState/Core.cpp
    MobileGL/MG_State/GLState/ErrorState/Error.cpp
    MobileGL/MG_State/GLState/BufferState/BufferState.cpp
    MobileGL/MG_State/GLState/BufferState/BufferObject.cpp
    MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.cpp
    MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.cpp
    MobileGL/MG_State/GLState/TextureState/MipmapStorage.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject1D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject2D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject2DCube.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject3D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObjectBuffer.cpp
    MobileGL/MG_State/GLState/TextureState/TextureUnit.cpp
    MobileGL/MG_State/GLState/TextureState/TextureState.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
    MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramState.cpp
    MobileGL/MG_State/GLState/RenderState/RenderState.cpp
    MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.cpp
    MobileGL/MG_State/GLState/FramebufferState/FramebufferState.cpp
    MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp
    MobileGL/MG_State/GLState/SamplerState/SamplerState.cpp
    MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp
    MobileGL/MG_State/GLState/RenderbufferState/RenderbufferState.cpp
)

if (APPLE AND NOT MOBILEGL_IOS)
    list(APPEND SOURCE_FILES
        MobileGL/MG_Impl/CGLImpl/CGLImpl.cpp
        MobileGL/MG_Impl/CGLImpl/Exporting/Definitions.cpp
        MobileGL/MG_Impl/DyldInterpose/DyldInterpose.cpp
        MobileGL/MG_Impl/NSOpenGLImpl/NSOpenGLImpl.cpp
    )
endif()

if (EMSCRIPTEN)
    # Emscripten/WebGPU build: drop the Vulkan + GLES backends and their loaders /
    # Vulkan enum converters, and add the DirectWebGPU backend plus the vendored
    # emdawnwebgpu C++ glue (webgpu.cpp).
    list(REMOVE_ITEM SOURCE_FILES
        MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
        MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp
        MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.cpp
        MobileGL/MG_Util/Converters/MGToVk/TextureEnumConverter.cpp
        MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
        MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
        MobileGL/MG_Backend/DirectGLES/Utils.cpp
        MobileGL/MG_Backend/DirectGLES/Managers.cpp
        MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
        MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp
        MobileGL/MG_Backend/DirectVulkan/VmaImpl.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/FrameContext.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/BufferArena.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateBuilder.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp
    )
    list(APPEND SOURCE_FILES
        MobileGL/MG_Backend/DirectWebGPU/BackendObject_DirectWebGPU.cpp
        MobileGL/MG_Backend/DirectWebGPU/DirectWebGPU.cpp
        MobileGL/MG_Backend/DirectWebGPU/Renderer/WebGPURenderer.cpp
        MobileGL/MG_Util/ShaderTranspiler/WgslTranspiler.cpp
        ${EMDAWNWEBGPU_WEBGPU_CPP}
    )

    # emscripten-glfw GL-backend adapter (mobilegl_webgl_* / mobilegl_get_proc_address).
    # Defines are scoped to the adapter TU so toggling the option doesn't recompile the archive.
    if (MOBILEGL_EMSCRIPTEN_GLFW)
        list(APPEND SOURCE_FILES MobileGL/MG_Impl/EmscriptenGLFW/EmscriptenGLFWBackend.cpp)
        set(_mobilegl_glfw_defs MOBILEGL_EMSCRIPTEN_GLFW)
        if (MOBILEGL_EMSCRIPTEN_GLFW_FPE)
            list(APPEND _mobilegl_glfw_defs MOBILEGL_EMSCRIPTEN_GLFW_FPE)
        endif()
        set_source_files_properties(MobileGL/MG_Impl/EmscriptenGLFW/EmscriptenGLFWBackend.cpp
            PROPERTIES COMPILE_DEFINITIONS "${_mobilegl_glfw_defs}")
    endif()

    # WgslTranspiler.cpp is the only TU that includes tint; compile it with tint's
    # ABI (C++20, no RTTI/exceptions) + the matching TINT_BUILD_* defines + the
    # prebuilt header bundle. Isolated here so the rest of MobileGL stays C++23.
    set_source_files_properties(MobileGL/MG_Util/ShaderTranspiler/WgslTranspiler.cpp
        PROPERTIES
        COMPILE_OPTIONS "-std=c++20;-fno-rtti;-fno-exceptions"
        INCLUDE_DIRECTORIES "${MOBILEGL_TINT_DIR}/include"
        COMPILE_DEFINITIONS "TINT_BUILD_SPV_READER=1;TINT_BUILD_WGSL_READER=1;TINT_BUILD_WGSL_WRITER=1;TINT_BUILD_SPV_WRITER=0;TINT_BUILD_GLSL_WRITER=0;TINT_BUILD_HLSL_WRITER=0;TINT_BUILD_MSL_WRITER=0;TINT_BUILD_IR_BINARY=0;TINT_BUILD_IS_LINUX=1;TINT_BUILD_IS_MAC=0;TINT_BUILD_IS_WIN=0")
endif()

set(MOBILEGL_LINK_LIBRARIES
        glslang::glslang
        spirv-cross-c
        SPIRV-Tools-opt
        SPIRV-Tools
        xxHash::xxhash
        spirv-reflect-static
)
if (NOT EMSCRIPTEN)
    list(APPEND MOBILEGL_LINK_LIBRARIES
        GPUOpen::VulkanMemoryAllocator
        Vulkan::UtilityHeaders
    )
else()
    list(APPEND MOBILEGL_LINK_LIBRARIES mobilegl_tint)
endif()

set(MOBILEGL_COMPILE_DEF
        -DVMA_STATIC_VULKAN_FUNCTIONS=0
        -DVMA_DYNAMIC_VULKAN_FUNCTIONS=1
        -DVMA_VULKAN_VERSION=1001000
)

message(STATUS "MOBILEGL_COMPILE_DEF=${MOBILEGL_COMPILE_DEF}")

set(MOBILEGL_INCLUDE_DIR
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/MobileGL
        ${spirv-tools_SOURCE_DIR}
        ${spirv-tools_SOURCE_DIR}/include
        ${spirv-tools_BINARY_DIR}
        ${SPIRV-Headers_SOURCE_DIR}/include
)
if (EMSCRIPTEN)
    # Fetched emdawnwebgpu headers. Placed first so the new standardized
    # <webgpu/webgpu.h> wins over Emscripten's built-in legacy copy in the sysroot.
    list(PREPEND MOBILEGL_INCLUDE_DIR ${EMDAWNWEBGPU_INCLUDE_DIRS})
endif()

if (EMSCRIPTEN)
    # Under Emscripten we want a static archive to link into the harness, not a
    # wasm side module — build the primary target STATIC and skip the MobileGL_s
    # duplicate below.
    add_library(${CMAKE_PROJECT_NAME} STATIC
        ${SOURCE_FILES}
    )
else()
    add_library(${CMAKE_PROJECT_NAME} SHARED
        ${SOURCE_FILES}
    )
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
    )
else()
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
    ${MOBILEGL_INCLUDE_DIR}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PUBLIC
    ${MOBILEGL_LINK_LIBRARIES}
)

target_compile_definitions(${CMAKE_PROJECT_NAME}
        PUBLIC
        ${MOBILEGL_COMPILE_DEF}
        MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL}
)

if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    foreach(MOBILEGL_LOADER_ALIAS
            libEGL.so libEGL.so.1)
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}>"
                    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${MOBILEGL_LOADER_ALIAS}"
            COMMENT "Creating ${MOBILEGL_LOADER_ALIAS} alias for Linux GL/EGL loaders"
        )
    endforeach()
endif()

if(NOT ANDROID AND NOT EMSCRIPTEN)
    add_library(${CMAKE_PROJECT_NAME}_s STATIC
        ${SOURCE_FILES}
    )

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_target_properties(${CMAKE_PROJECT_NAME}_s PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
            VISIBILITY_INLINES_HIDDEN OFF
        )
    else()
        set_target_properties(${CMAKE_PROJECT_NAME}_s PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    endif()

    target_include_directories(${CMAKE_PROJECT_NAME}_s PUBLIC
        ${MOBILEGL_INCLUDE_DIR}
    )

    target_link_libraries(${CMAKE_PROJECT_NAME}_s
        PUBLIC
        ${MOBILEGL_LINK_LIBRARIES}
    )

    target_compile_definitions(${CMAKE_PROJECT_NAME}_s
            PUBLIC
            ${MOBILEGL_COMPILE_DEF}
            MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL}
    )
endif()

if (TRACY_ENABLE)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC Tracy::TracyClient)
    target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC Tracy::TracyClient)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC -DTRACY_ENABLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_s PUBLIC -DTRACY_ENABLE)
endif ()

if (ANDROID)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        android
        log
        vulkan
    )
endif()

if (APPLE AND NOT MOBILEGL_IOS)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework OpenGL"
        objc)
    if(TARGET ${CMAKE_PROJECT_NAME}_s)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC
            "-framework Cocoa"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework OpenGL"
            objc)
    endif()
endif()

if (APPLE AND MOBILEGL_IOS)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC MOBILEGL_IOS=1 _LIBCPP_DISABLE_AVAILABILITY)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        "-framework CoreGraphics"
        "-framework Foundation"
        "-framework QuartzCore"
        objc)
    if (MOBILEGL_VULKAN_LIBRARY)
        target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC "${MOBILEGL_VULKAN_LIBRARY}")
    endif()

    if(TARGET ${CMAKE_PROJECT_NAME}_s)
        target_compile_definitions(${CMAKE_PROJECT_NAME}_s PUBLIC MOBILEGL_IOS=1 _LIBCPP_DISABLE_AVAILABILITY)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC
            "-framework CoreGraphics"
            "-framework Foundation"
            "-framework QuartzCore"
            objc)
        if (MOBILEGL_VULKAN_LIBRARY)
            target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC "${MOBILEGL_VULKAN_LIBRARY}")
        endif()
    endif()
endif()

if (NOT ANDROID AND NOT MOBILEGL_IOS AND NOT EMSCRIPTEN)
    find_package(Vulkan)
    if (Vulkan_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC Vulkan::Vulkan Vulkan::Headers)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC Vulkan::Vulkan Vulkan::Headers)
        target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${Vulkan_INCLUDE_DIR})
        target_include_directories(${CMAKE_PROJECT_NAME}_s PUBLIC ${Vulkan_INCLUDE_DIR})
    endif ()
endif ()

if (NOT ANDROID)
    if (MOBILEGL_BUILD_TEST)
        add_subdirectory(MobileGL/MG_Test)
    endif()

    if (MOBILEGL_BUILD_BENCHMARK)
        add_subdirectory(MobileGL/MG_Benchmark)
    endif()

    if (MOBILEGL_BUILD_TRACE_REPLAY)
        enable_testing()
        add_subdirectory(tools/trace_replay)
    endif()
endif()

if (EMSCRIPTEN)
    # Standalone DirectWebGPU web harness (drives the real GL/EGL frontend in-browser).
    option(MOBILEGL_BUILD_WEB_HARNESS "Build the standalone DirectWebGPU web harness" OFF)
    if (MOBILEGL_BUILD_WEB_HARNESS)
        add_executable(mobilegl_web_harness ${CMAKE_SOURCE_DIR}/tools/web_harness/main.cpp)
        target_link_libraries(mobilegl_web_harness PRIVATE ${CMAKE_PROJECT_NAME})
        set(_emdawn_js_flags)
        foreach(_js ${EMDAWNWEBGPU_JS_LIBRARIES})
            list(APPEND _emdawn_js_flags "--js-library=${_js}")
        endforeach()
        target_link_options(mobilegl_web_harness PRIVATE
            ${_emdawn_js_flags}
            # JSPI suspend shim for synchronous glReadPixels/glFinish (M4).
            "--js-library=${CMAKE_SOURCE_DIR}/MobileGL/MG_Backend/DirectWebGPU/lib_mobilegl_webgpu.js"
            "-sWASM_BIGINT" "-sSTACK_SIZE=16MB" "-sALLOW_MEMORY_GROWTH=1" "-sEXIT_RUNTIME=0"
            "-sEXPORTED_RUNTIME_METHODS=addRunDependency,removeRunDependency"
            # Keep main + the JSPI-wrapped readback entry in wasmExports so the shell
            # can drive it through WebAssembly.promising.
            "-sEXPORTED_FUNCTIONS=_main,_harness_readback"
            # Synchronous instantiation assigns exports eagerly (no lazy JS wrapper),
            # so Module['_harness_readback'] is the raw WebAssembly export that
            # WebAssembly.promising requires. Note: Chrome refuses synchronous wasm
            # compile above 8MB, so debug/name-section builds must go async (and then
            # the JSPI capture entry cannot be wrapped — replay-only debugging).
            "-sWASM_ASYNC_COMPILATION=0"
            "--shell-file=${CMAKE_SOURCE_DIR}/tools/web_harness/shell.html")
        set_target_properties(mobilegl_web_harness PROPERTIES SUFFIX ".html")
    endif()

    # Threaded harness: exercises the FULL ikvmcraft pipeline (pthreads + PROXY_TO_PTHREAD +
    # a transferred OffscreenCanvas owned by the render worker) through the mobilegl_* adapter.
    # Requires MOBILEGL_EMSCRIPTEN_GLFW (the adapter) and a -pthread build of MobileGL.
    option(MOBILEGL_BUILD_WEB_HARNESS_THREADED "Build the threaded (pthread) DirectWebGPU harness" OFF)
    if (MOBILEGL_BUILD_WEB_HARNESS_THREADED)
        if (NOT MOBILEGL_EMSCRIPTEN_GLFW)
            message(FATAL_ERROR "MOBILEGL_BUILD_WEB_HARNESS_THREADED requires -DMOBILEGL_EMSCRIPTEN_GLFW=ON")
        endif()
        add_executable(mobilegl_web_harness_threaded ${CMAKE_SOURCE_DIR}/tools/web_harness/main_threaded.cpp)
        target_link_libraries(mobilegl_web_harness_threaded PRIVATE ${CMAKE_PROJECT_NAME})
        set(_emdawn_js_flags_mt)
        foreach(_js ${EMDAWNWEBGPU_JS_LIBRARIES})
            list(APPEND _emdawn_js_flags_mt "--js-library=${_js}")
        endforeach()
        target_link_options(mobilegl_web_harness_threaded PRIVATE
            ${_emdawn_js_flags_mt}
            "--js-library=${CMAKE_SOURCE_DIR}/MobileGL/MG_Backend/DirectWebGPU/lib_mobilegl_webgpu.js"
            "-pthread"
            "-sPROXY_TO_PTHREAD=1"
            "-sOFFSCREENCANVAS_SUPPORT=1"
            "-sOFFSCREENCANVASES_TO_PTHREAD=.canvas"
            "-sPTHREAD_POOL_SIZE=4"
            "-sWASM_BIGINT" "-sSTACK_SIZE=16MB" "-sALLOW_MEMORY_GROWTH=1" "-sEXIT_RUNTIME=0"
            "-sEXPORTED_RUNTIME_METHODS=ccall"
            "-sEXPORTED_FUNCTIONS=_main,_run_render"
            "--shell-file=${CMAKE_SOURCE_DIR}/tools/web_harness/shell_threaded.html")
        set_target_properties(mobilegl_web_harness_threaded PROPERTIES SUFFIX ".html")
    endif()

    # Part-A validation probe: links MobileGL + the symbol-renamed SimpleFPEWrapper archive and
    # exercises the mobilegl_* adapter + SFPEW proc-address routing on the main thread. Point
    # -DMOBILEGL_SFPEW_ARCHIVE at the renamed libSimpleFPEWrapper.a.
    option(MOBILEGL_BUILD_FPE_PROBE "Build the FPE adapter link/routing probe" OFF)
    set(MOBILEGL_SFPEW_ARCHIVE "" CACHE FILEPATH "Path to the symbol-renamed libSimpleFPEWrapper.a")
    if (MOBILEGL_BUILD_FPE_PROBE)
        if (NOT MOBILEGL_EMSCRIPTEN_GLFW OR NOT EXISTS "${MOBILEGL_SFPEW_ARCHIVE}")
            message(FATAL_ERROR "MOBILEGL_BUILD_FPE_PROBE needs MOBILEGL_EMSCRIPTEN_GLFW=ON and a valid MOBILEGL_SFPEW_ARCHIVE")
        endif()
        add_library(sfpew_renamed STATIC IMPORTED)
        set_target_properties(sfpew_renamed PROPERTIES IMPORTED_LOCATION "${MOBILEGL_SFPEW_ARCHIVE}")
        add_executable(mobilegl_fpe_probe ${CMAKE_SOURCE_DIR}/tools/web_harness/main_fpe_probe.cpp)
        target_link_libraries(mobilegl_fpe_probe PRIVATE ${CMAKE_PROJECT_NAME} sfpew_renamed)
        set(_emdawn_js_flags_probe)
        foreach(_js ${EMDAWNWEBGPU_JS_LIBRARIES})
            list(APPEND _emdawn_js_flags_probe "--js-library=${_js}")
        endforeach()
        target_link_options(mobilegl_fpe_probe PRIVATE
            ${_emdawn_js_flags_probe}
            "--js-library=${CMAKE_SOURCE_DIR}/MobileGL/MG_Backend/DirectWebGPU/lib_mobilegl_webgpu.js"
            "-sWASM_BIGINT" "-sSTACK_SIZE=16MB" "-sALLOW_MEMORY_GROWTH=1" "-sEXIT_RUNTIME=0"
            "-sEXPORTED_RUNTIME_METHODS=addRunDependency,removeRunDependency"
            "-sEXPORTED_FUNCTIONS=_main"
            "-sWASM_ASYNC_COMPILATION=0"
            "--shell-file=${CMAKE_SOURCE_DIR}/tools/web_harness/shell.html")
        set_target_properties(mobilegl_fpe_probe PROPERTIES SUFFIX ".html")
    endif()

    # Browser-side apitrace replayer: replays a recorded GL trace through MobileGL ->
    # DirectWebGPU and reads back the framebuffer at a target call (golden compare).
    # Configure with -DMOBILEGL_TRACE_FILE=/abs/path/to/trace.trace (preloaded into the FS).
    option(MOBILEGL_BUILD_TRACE_REPLAY_WEB "Build the browser-side apitrace replayer" OFF)
    if (MOBILEGL_BUILD_TRACE_REPLAY_WEB)
        set(_ap ${CMAKE_SOURCE_DIR}/3rdparty/apitrace)
        set(_sn ${_ap}/thirdparty/snappy)
        # Generate snappy-stubs-public.h (Linux substitutions) into the build dir.
        file(READ "${_sn}/snappy-stubs-public.h.in" _snhdr)
        string(REPLACE "\${HAVE_SYS_UIO_H_01}" "1" _snhdr "${_snhdr}")
        string(REPLACE "\${PROJECT_VERSION_MAJOR}" "1" _snhdr "${_snhdr}")
        string(REPLACE "\${PROJECT_VERSION_MINOR}" "1" _snhdr "${_snhdr}")
        string(REPLACE "\${PROJECT_VERSION_PATCH}" "9" _snhdr "${_snhdr}")
        file(WRITE "${CMAKE_BINARY_DIR}/apitrace_gen/snappy-stubs-public.h" "${_snhdr}")

        add_executable(mobilegl_trace_replay_web
            ${CMAKE_SOURCE_DIR}/tools/web_harness/trace_replay_web.cpp
            # apitrace parser (snappy-only; createForRead provided in the cpp above)
            ${_ap}/lib/trace/trace_parser.cpp
            ${_ap}/lib/trace/trace_parser_flags.cpp
            ${_ap}/lib/trace/trace_model.cpp
            ${_ap}/lib/trace/trace_file.cpp
            ${_ap}/lib/trace/trace_file_snappy.cpp
            ${_ap}/lib/trace/trace_option.cpp
            ${_ap}/lib/os/os_posix.cpp
            ${_ap}/lib/os/os_backtrace.cpp
            ${_sn}/snappy.cc ${_sn}/snappy-sinksource.cc ${_sn}/snappy-stubs-internal.cc)
        target_include_directories(mobilegl_trace_replay_web PRIVATE
            ${_ap}/lib/trace ${_ap}/lib/os ${_ap}/lib/common ${_ap}/lib/highlight
            ${_ap}/thirdparty/zstd/contrib/seekable_format ${_sn}
            ${CMAKE_BINARY_DIR}/apitrace_gen)
        # apitrace is C++17 (a C++20 build hits a deleted ostream<<wchar_t* overload in
        # dead trace_parser code); keep RTTI/exceptions; PATH_MAX for os_posix on wasm.
        target_compile_options(mobilegl_trace_replay_web PRIVATE
            -std=c++17 -fexceptions -frtti -DPATH_MAX=4096 -Wno-deprecated-declarations -Wno-\#warnings)
        target_link_libraries(mobilegl_trace_replay_web PRIVATE ${CMAKE_PROJECT_NAME})
        set(_rep_js_flags)
        foreach(_js ${EMDAWNWEBGPU_JS_LIBRARIES})
            list(APPEND _rep_js_flags "--js-library=${_js}")
        endforeach()
        if (NOT MOBILEGL_TRACE_FILE)
            set(MOBILEGL_TRACE_FILE "" CACHE STRING "Absolute path to a .trace to preload as /trace.trace")
        endif()
        set(_preload)
        if (MOBILEGL_TRACE_FILE)
            set(_preload "--preload-file=${MOBILEGL_TRACE_FILE}@/trace.trace")
        endif()
        target_link_options(mobilegl_trace_replay_web PRIVATE
            ${_rep_js_flags}
            "--js-library=${CMAKE_SOURCE_DIR}/MobileGL/MG_Backend/DirectWebGPU/lib_mobilegl_webgpu.js"
            "-sWASM_BIGINT" "-sSTACK_SIZE=32MB" "-sALLOW_MEMORY_GROWTH=1" "-sEXIT_RUNTIME=0"
            "-sFORCE_FILESYSTEM=1"
            "-sEXPORTED_RUNTIME_METHODS=addRunDependency,removeRunDependency,HEAPU8,FS"
            "-sEXPORTED_FUNCTIONS=_main,_replay_run,_replay_capture,_replay_done,_replay_pixels,_replay_width,_replay_height"
            # Synchronous instantiation assigns exports eagerly (no lazy JS wrapper),
            # so Module['_replay_capture'] is the raw WebAssembly export that
            # WebAssembly.promising requires. Note: Chrome refuses synchronous wasm
            # compile above 8MB, so debug/name-section builds must go async (and then
            # the JSPI capture entry cannot be wrapped — replay-only debugging).
            "-sWASM_ASYNC_COMPILATION=0" "-fexceptions"
            ${_preload}
            "--shell-file=${CMAKE_SOURCE_DIR}/tools/web_harness/replay_shell.html")
        set_target_properties(mobilegl_trace_replay_web PROPERTIES SUFFIX ".html")
    endif()
endif()
