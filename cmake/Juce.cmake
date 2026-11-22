# JUCE 9.0.6, pinned as the git submodule third_party/JUCE (commit 654bdbc7a9db0dae90849b44abe91f339e9f0768).
# Licence: AGPLv3 / commercial — see THIRD_PARTY.md.
#
# JUCE 9 added OpenGL ES on Linux (EGL on X11), which makes JUCE's OpenGLContext usable on the device's GC2000.
# Device modules: juce_core, juce_events, juce_data_structures, juce_graphics (fonts, glyph atlas),
# juce_audio_basics + juce_audio_devices (ALSA), juce_gui_basics + juce_opengl (the juce-gl renderer backend).
# JUCE_OPENGL_ES=1 is set directly instead of JUCE's NEEDS_OPENGL_ES target option: that option links libEGL and
# libGLESv2 at build time, while the device needs the Vivante -x11 flavour chosen at run time (gldispatch).
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt)
    message(FATAL_ERROR "JUCE is missing: git submodule update --init --depth 1 third_party/JUCE")
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/JUCE ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)

# Definitions every JUCE-using target gets. The device has a bare X server (no window manager, no Xinerama, Xcursor,
# XShm, XRandR), no libcurl and no GTK (findings of an earlier JUCE proof of concept).
add_library(app_juce_config INTERFACE)
target_compile_definitions(app_juce_config INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=$<BOOL:${APP_BUILD_DEVICE_APP}>
    JUCE_USE_XSHM=0
    JUCE_USE_XRANDR=0
    JUCE_USE_XINERAMA=0
    JUCE_USE_XCURSOR=0
    JUCE_USE_XRENDER=0
    JUCE_ALSA=1
    JUCE_JACK=0
    JUCE_USE_MP3AUDIOFORMAT=0)

# One static library compiles the JUCE modules once; every target that needs JUCE links app::juce (JUCE's documented
# pattern for sharing modules between targets). The definitions and include paths propagate to dependants.
function(app_add_juce_library name)
    add_library(${name} STATIC)
    target_link_libraries(${name}
        PRIVATE ${ARGN}
        PUBLIC app_juce_config juce::juce_recommended_config_flags)
    target_compile_definitions(${name} PUBLIC JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(${name} SYSTEM INTERFACE $<TARGET_PROPERTY:${name},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${name} INTERFACE $<TARGET_PROPERTY:${name},COMPILE_DEFINITIONS>)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    app_release_o3(${name}) # JUCE's modules are compiled with -O3 in Release too
endfunction()

# Host and device: the modules the libraries need (font rasterisation for the glyph atlas, image codecs for snapshots).
set(APP_JUCE_MODULES juce::juce_core juce::juce_events juce::juce_graphics juce::juce_data_structures)
if(APP_BUILD_DEVICE_APP)
    # MODAL_LOOPS: the app's own frame loop pumps JUCE messages (runDispatchLoopUntil) for the juce-gl backend.
    list(APPEND APP_JUCE_MODULES juce::juce_audio_basics juce::juce_audio_devices juce::juce_gui_basics juce::juce_opengl)
    target_compile_definitions(app_juce_config INTERFACE JUCE_OPENGL_ES=1)
else()
    # Host only: SVG rasterisation of the mockups for the snapshot tests (juce::Drawable).
    list(APPEND APP_JUCE_MODULES juce::juce_gui_basics)
endif()
app_add_juce_library(app_juce ${APP_JUCE_MODULES})
add_library(app::juce ALIAS app_juce)

# Embedded fonts: the device has no font files. Inter, JetBrains Mono and the DejaVu symbol subset are compiled
# into the ui library (ui/render/fonts/FontData.cpp); app_fonts stays as an empty target so existing
# target_link_libraries(... app_fonts) lines keep linking.
add_library(app_fonts INTERFACE)
