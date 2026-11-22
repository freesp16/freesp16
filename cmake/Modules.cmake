# Module libraries with auto-discovered sources, and whole-archive linking for static registrars.
#
#   app_add_module(<module> [REALTIME] [EXCLUDE <subdir>...] [DEVICE_ONLY <subdir>...] [HOST_ONLY <subdir>...]
#                  [LINUX_ONLY <subdir>...])
#
# Creates the static library <prefix>_<module> (alias app::<module>) from every *.cpp below the calling
# CMakeLists.txt's folder (file(GLOB_RECURSE ... CONFIGURE_DEPENDS), so a new file in a new sub-folder is picked up by
# the next build without editing any CMake list — a new feature never has to touch a shared CMakeLists.txt).
#   EXCLUDE      sub-folders (relative to the module folder) never compiled into the library
#   DEVICE_ONLY  sub-folders compiled only into the device build (APP_BUILD_DEVICE_APP=ON)
#   HOST_ONLY    sub-folders compiled only into host builds
#   LINUX_ONLY   sub-folders compiled only when the target system is Linux (the device, a Linux desktop host; not a
#                macOS host). Combines with the others: HOST_ONLY + LINUX_ONLY = Linux hosts only.
#   REALTIME     the module is real-time code (app_target_defaults(... REALTIME), tools/rt-lint.py scans it)
# A module whose folder holds no source yet still gets a library (from a generated empty translation unit), so its
# dependants link and an empty folder is never an error.
#
#   app_link_modules(<target> <module-target>...)
#
# Links module libraries with $<LINK_LIBRARY:WHOLE_ARCHIVE,...>: every object file is kept, so the static registrars
# behind APP_REGISTER_* (platforms, renderers, screens, widgets, effects, stretchers, codecs, control sources,
# parameter catalogues) run even though nothing references them by name. LINK_LIBRARY_OVERRIDE_<lib> makes the
# feature win when the same library also arrives as a plain transitive dependency. Use it for every executable.

function(app_add_module module)
    cmake_parse_arguments(ARG "REALTIME" "" "EXCLUDE;DEVICE_ONLY;HOST_ONLY;LINUX_ONLY" ${ARGN})
    set(target ${APP_TARGET_PREFIX}_${module})
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
    set(skip ${ARG_EXCLUDE})
    if(APP_BUILD_DEVICE_APP)
        list(APPEND skip ${ARG_HOST_ONLY})
    else()
        list(APPEND skip ${ARG_DEVICE_ONLY})
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND skip ${ARG_LINUX_ONLY})
    endif()
    foreach(dir IN LISTS skip)
        list(FILTER sources EXCLUDE REGEX "^${dir}/")
    endforeach()
    list(TRANSFORM sources PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
    if(NOT sources)
        set(stub ${CMAKE_CURRENT_BINARY_DIR}/${module}_empty_module.cpp)
        file(CONFIGURE OUTPUT ${stub} CONTENT "// Generated: the ${module} module has no sources yet.\n")
        set(sources ${stub})
    endif()
    add_library(${target} STATIC ${sources})
    add_library(app::${module} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/src) # include root: "engine/EngineApi.h"
    if(ARG_REALTIME)
        app_target_defaults(${target} REALTIME)
    else()
        app_target_defaults(${target})
    endif()
endfunction()

function(app_link_modules target)
    foreach(lib IN LISTS ARGN)
        if(TARGET ${lib})
            get_target_property(real ${lib} ALIASED_TARGET)
            if(NOT real)
                set(real ${lib})
            endif()
            target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${real}>")
            set_property(TARGET ${target} PROPERTY LINK_LIBRARY_OVERRIDE_${real} WHOLE_ARCHIVE)
        endif()
    endforeach()
endfunction()
