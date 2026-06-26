cmake_minimum_required(VERSION 3.22.1)

project("MobileGL")

option(MOBILEGL_BUILD_TEST        "Build MobileGL tests"                             ON )
option(MOBILEGL_BUILD_BENCHMARK   "Build MobileGL benchmarks"                        ON )
option(MOBILEGL_FORCE_RELEASE_OPT "Enable Release optimization flags in Debug build" ON )
option(MOBILEGL_ENABLE_TRACY      "Enable tracy for profiling"                       OFF)
option(MOBILEGL_BUILD_TRACE_REPLAY "Build desktop apitrace replay runner"            OFF)
option(MOBILEGL_IOS               "Build MobileGL for iOS instead of macOS when APPLE is set" OFF)
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

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
            # WebAssembly.promising requires.
            "-sWASM_ASYNC_COMPILATION=0"
            "--shell-file=${CMAKE_SOURCE_DIR}/tools/web_harness/shell.html")
        set_target_properties(mobilegl_web_harness PROPERTIES SUFFIX ".html")
    endif()
endif()
