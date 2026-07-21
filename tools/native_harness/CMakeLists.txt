find_package(X11 REQUIRED)

add_library(mobilegl_glx_bridge SHARED glx_bridge.cpp)
target_compile_features(mobilegl_glx_bridge PRIVATE cxx_std_20)
target_link_libraries(mobilegl_glx_bridge PRIVATE ${CMAKE_PROJECT_NAME} X11::X11 ${CMAKE_DL_LIBS})
target_include_directories(mobilegl_glx_bridge PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(UNIX AND NOT APPLE)
    # When apitrace is preloaded first, references such as &glXSwapIntervalEXT
    # inside this bridge must resolve to the bridge implementation, not back to
    # apitrace's interposing wrapper.
    target_link_options(mobilegl_glx_bridge PRIVATE "-Wl,-Bsymbolic")
endif()
set_target_properties(mobilegl_glx_bridge PROPERTIES OUTPUT_NAME MobileGLGLX)
add_custom_command(TARGET mobilegl_glx_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:mobilegl_glx_bridge>"
            "$<TARGET_FILE_DIR:mobilegl_glx_bridge>/libGL.so.1"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:mobilegl_glx_bridge>"
            "$<TARGET_FILE_DIR:mobilegl_glx_bridge>/libGL.so"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:mobilegl_glx_bridge>"
            "$<TARGET_FILE_DIR:mobilegl_glx_bridge>/libGLX.so.0"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:mobilegl_glx_bridge>"
            "$<TARGET_FILE_DIR:mobilegl_glx_bridge>/libGLX.so"
    COMMENT "Creating libGL and libGLX aliases for GLFW/LWJGL dlopen interception")
