# The app executable: the composition root only. Plain add_executable: the frame loop is the app's own; JUCE windows
# are created only by the juce-gl backend.
#
# Sources: every *.cpp below app/ is compiled automatically, except preview/** (HOST_ONLY: the host preview
# window). The device build is the product (armel); the host build (APP_BUILD_HOST_APP) is the same composition root
# on the host platform (offline audio, offscreen display), used by the app smoke test in tests/.
#
# Every module library is linked whole-archive (app_link_modules) so the static registrars behind APP_REGISTER_* run.
#
# GL dispatch (device): every egl*/gl*/fb* symbol referenced by the libraries below (JUCE's juce_opengl, the custom
# GLES2 renderer, the imx6 display) gets a generated ARM stub that jumps through a table bound at run time to one
# Vivante flavour (platform/common/display/gldispatch, tools/gen-gl-dispatch.py). So no libEGL/libGLESv2 appears in
# DT_NEEDED. GlDispatch.cpp is compiled here, not in the platform library, because it includes the generated list.
# Linux desktop host: the same scheme with x86-64 stubs over the GLES2 renderer library (<prefix>_ui_gles)
# and the Mesa flavour (libEGL.so.1 + libGLESv2.so.2) at run time; the host app links no GL library either.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER APP_SOURCES EXCLUDE REGEX "/app/preview/")
add_executable(${PROJECT_IDENTITY_NAME} ${APP_SOURCES})
app_link_modules(${PROJECT_IDENTITY_NAME} app::platform app::ui app::services app::io app::engine app::core)
target_link_libraries(${PROJECT_IDENTITY_NAME} PRIVATE ${CMAKE_DL_LIBS})
app_target_defaults(${PROJECT_IDENTITY_NAME})
set_target_properties(${PROJECT_IDENTITY_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The unit launcher reads the app icon and metadata from an ELF note.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(APP_INFO_SOURCE ${APP_GENERATED_DIR}/AppInfoNote.cpp)
    add_custom_command(
        OUTPUT ${APP_INFO_SOURCE}
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-app-info.py ${CMAKE_SOURCE_DIR}/resources/icons/app-icon.png
                ${APP_INFO_SOURCE} ${PROJECT_IDENTITY_NAME} ${PROJECT_IDENTITY_VERSION} ${PROJECT_IDENTITY_ID}
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen-app-info.py ${CMAKE_SOURCE_DIR}/resources/icons/app-icon.png
        COMMENT "Generating the app-info note"
        VERBATIM)
    target_sources(${PROJECT_IDENTITY_NAME} PRIVATE ${APP_INFO_SOURCE})
endif()

if(APP_BUILD_DEVICE_APP)
    set(APP_GLDISPATCH_SRC ${CMAKE_SOURCE_DIR}/src/platform/common/display/gldispatch)
    set(APP_GLDISPATCH_DIR ${APP_GENERATED_DIR}/gldispatch)
    set(APP_GLDISPATCH_LIBS $<TARGET_FILE:app_juce> $<TARGET_FILE:${APP_TARGET_PREFIX}_ui>
                            $<TARGET_FILE:${APP_TARGET_PREFIX}_platform>)
    add_custom_command(
        OUTPUT ${APP_GLDISPATCH_DIR}/gldispatch_stubs.S ${APP_GLDISPATCH_DIR}/gldispatch_names.inc
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py ${CMAKE_NM} ${APP_GLDISPATCH_DIR}
                ${APP_GLDISPATCH_LIBS}
        DEPENDS app_juce ${APP_TARGET_PREFIX}_ui ${APP_TARGET_PREFIX}_platform ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py
        COMMENT "Generating GL dispatch stubs"
        VERBATIM)
    enable_language(ASM)
    target_sources(${PROJECT_IDENTITY_NAME} PRIVATE ${APP_GLDISPATCH_SRC}/GlDispatch.cpp
                                                    ${APP_GLDISPATCH_DIR}/gldispatch_stubs.S)
    target_include_directories(${PROJECT_IDENTITY_NAME} PRIVATE ${APP_GLDISPATCH_DIR})
endif()

if(NOT APP_BUILD_DEVICE_APP AND TARGET app::ui_gles)
    app_link_modules(${PROJECT_IDENTITY_NAME} app::ui_gles)
    set(APP_GLDISPATCH_SRC ${CMAKE_SOURCE_DIR}/src/platform/common/display/gldispatch)
    set(APP_GLDISPATCH_DIR ${APP_GENERATED_DIR}/gldispatch)
    set(APP_GLDISPATCH_LIBS $<TARGET_FILE:${APP_TARGET_PREFIX}_ui_gles> $<TARGET_FILE:${APP_TARGET_PREFIX}_platform>)
    add_custom_command(
        OUTPUT ${APP_GLDISPATCH_DIR}/gldispatch_stubs.S ${APP_GLDISPATCH_DIR}/gldispatch_names.inc
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py --arch x86_64 ${CMAKE_NM}
                ${APP_GLDISPATCH_DIR} ${APP_GLDISPATCH_LIBS}
        DEPENDS ${APP_TARGET_PREFIX}_ui_gles ${APP_TARGET_PREFIX}_platform ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py
        COMMENT "Generating GL dispatch stubs (x86-64)"
        VERBATIM)
    enable_language(ASM)
    target_sources(${PROJECT_IDENTITY_NAME} PRIVATE ${APP_GLDISPATCH_SRC}/GlDispatch.cpp
                                                    ${APP_GLDISPATCH_DIR}/gldispatch_stubs.S)
    target_include_directories(${PROJECT_IDENTITY_NAME} PRIVATE ${APP_GLDISPATCH_DIR})
endif()

# Host preview window: host builds only.
if(NOT APP_BUILD_DEVICE_APP)
    add_subdirectory(preview)
endif()
