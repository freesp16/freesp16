# The preview target (included by app/preview/CMakeLists.txt or by preview-hook.cmake). Paths are absolute so the file
# works from either place.
set(APP_PREVIEW_TARGET ${PROJECT_IDENTITY_NAME}_preview)
add_executable(${APP_PREVIEW_TARGET} ${CMAKE_SOURCE_DIR}/src/app/preview/PreviewMain.cpp)
app_link_modules(${APP_PREVIEW_TARGET} app::platform app::ui app::services app::io app::engine app::core)
target_link_libraries(${APP_PREVIEW_TARGET} PRIVATE app::juce app_fonts)
target_include_directories(${APP_PREVIEW_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src ${APP_GENERATED_DIR})
app_target_defaults(${APP_PREVIEW_TARGET})
set_target_properties(${APP_PREVIEW_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
