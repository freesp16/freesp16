# Host preview window: a JUCE DocumentWindow that runs the Shell (HOME and every registered screen) on the `soft`
# backend, with the computer keyboard and the mouse mapped to logical control events (03-ui/control-surface.md
# UI-120…123). Host only: app/CMakeLists.txt excludes app/preview/** from the device app and adds this directory on
# host builds (`if(NOT APP_BUILD_DEVICE_APP) add_subdirectory(preview) endif()`); without that, configure with
# -DCMAKE_PROJECT_INCLUDE=app/preview/preview-hook.cmake.
#   cmake --build --preset host --target <name>_preview && build/host/bin/<name>_preview
include(${CMAKE_CURRENT_SOURCE_DIR}/Preview.cmake)
