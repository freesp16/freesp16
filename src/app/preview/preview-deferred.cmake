# Deferred part of preview-hook.cmake: adds the preview target unless app/CMakeLists.txt already did.
if(NOT APP_BUILD_DEVICE_APP AND NOT TARGET ${PROJECT_IDENTITY_NAME}_preview)
    include(${CMAKE_SOURCE_DIR}/src/app/preview/Preview.cmake)
endif()
