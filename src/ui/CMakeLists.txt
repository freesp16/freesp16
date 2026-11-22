# ui: theme (generated from tokens.json), descriptors, render backends, widgets, screens, shell. Message thread only.
#
# Sources: every *.cpp below this folder is compiled into <prefix>_ui automatically (cmake/Modules.cmake), except:
#   render/gles/**    DEVICE_ONLY  custom GLES2 renderer (gl-fb, gl-x11): EGL/GLES headers from the device sysroot
#   render/jucegl/**  DEVICE_ONLY  JUCE 9 OpenGLContext backend (juce-gl): juce_opengl is a device-only module
# No GL library is linked: the device executable carries the gldispatch stubs (app/, platform/common/display/gldispatch).
#
# Linux desktop host: render/gles is built into its own library <prefix>_ui_gles (system EGL/GLES2 headers,
# libegl-dev + libgles-dev), linked only into the app executable, which carries the x86-64 gldispatch stubs. The test
# executables link <prefix>_ui without it, so they need no GL symbols. render/jucegl stays device-only (the host JUCE
# library has no juce_opengl).
# Backends register themselves (APP_REGISTER_RENDERER), screens/widgets too (APP_REGISTER_SCREEN/WIDGET): nothing to
# list here.
app_add_module(ui DEVICE_ONLY render/gles render/jucegl)
add_dependencies(${APP_TARGET_PREFIX}_ui app_theme)
target_include_directories(${APP_TARGET_PREFIX}_ui PUBLIC ${APP_GENERATED_DIR})
target_link_libraries(${APP_TARGET_PREFIX}_ui PUBLIC app::io app::core app::juce app_fonts)

if(NOT APP_BUILD_DEVICE_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(APP_EGL_INCLUDE_DIR EGL/egl.h)
    find_path(APP_GLES2_INCLUDE_DIR GLES2/gl2.h)
    if(APP_EGL_INCLUDE_DIR AND APP_GLES2_INCLUDE_DIR)
        add_library(${APP_TARGET_PREFIX}_ui_gles STATIC ${CMAKE_CURRENT_SOURCE_DIR}/render/gles/GlesRenderer.cpp)
        add_library(app::ui_gles ALIAS ${APP_TARGET_PREFIX}_ui_gles)
        target_include_directories(${APP_TARGET_PREFIX}_ui_gles PRIVATE ${APP_EGL_INCLUDE_DIR} ${APP_GLES2_INCLUDE_DIR})
        target_link_libraries(${APP_TARGET_PREFIX}_ui_gles PUBLIC app::ui)
        app_target_defaults(${APP_TARGET_PREFIX}_ui_gles)
    else()
        message(STATUS "EGL/GLES2 headers not found: the Linux host app is built without the gl-x11 renderer")
    endif()
endif()
