# Builds the host preview without editing app/CMakeLists.txt:
#   cmake --preset host -DCMAKE_PROJECT_INCLUDE=$PWD/app/preview/preview-hook.cmake
# Deferred to the end of the top-level CMakeLists.txt, after the module functions and targets exist. Host only.
get_property(app_preview_hooked GLOBAL PROPERTY APP_PREVIEW_HOOKED)
if(NOT app_preview_hooked AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set_property(GLOBAL PROPERTY APP_PREVIEW_HOOKED TRUE)
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL include ${CMAKE_SOURCE_DIR}/src/app/preview/preview-deferred.cmake)
endif()
