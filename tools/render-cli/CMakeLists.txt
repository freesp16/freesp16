# <name>_render_cli (spec 01-architecture/module-layout.md "<name>_render_cli: offline render of a project file → WAV
# (used by tests)"): the render service's synchronous core over a project bundle, with the app's own project forwarding
# (src/app/ProjectFeed.cpp is compiled in: the app executable is not a library).
#
# Host builds only for now: the module libraries are linked whole-archive like the app (the parameter catalogues,
# effects and codecs register themselves), and on the device the ui library references the GL symbols that only the
# app's generated GL dispatch stubs provide (src/app/CMakeLists.txt). The device-side comparison (a host render vs a
# device render within −100 dBFS) needs those stubs or a ui-free feeder first (not done yet).
add_executable(${APP_TARGET_PREFIX}_render_cli main.cpp ${CMAKE_SOURCE_DIR}/src/app/ProjectFeed.cpp)
app_link_modules(${APP_TARGET_PREFIX}_render_cli app::ui app::services app::io app::engine app::core)
target_link_libraries(${APP_TARGET_PREFIX}_render_cli PRIVATE app::juce app_fonts)
target_include_directories(${APP_TARGET_PREFIX}_render_cli PRIVATE ${CMAKE_SOURCE_DIR}/src ${APP_GENERATED_DIR})
app_target_defaults(${APP_TARGET_PREFIX}_render_cli)
set_target_properties(${APP_TARGET_PREFIX}_render_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
