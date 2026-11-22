cmake_minimum_required(VERSION 3.25)

# The project's name, display name, id and version live only in cmake/ProjectIdentity.cmake.
include(cmake/ProjectIdentity.cmake)
project(${PROJECT_IDENTITY_NAME} VERSION ${PROJECT_IDENTITY_VERSION} LANGUAGES C CXX)

string(TOUPPER "${PROJECT_IDENTITY_NAME}" PROJECT_IDENTITY_ENV_PREFIX)
string(REPLACE "-" "_" PROJECT_IDENTITY_ENV_PREFIX "${PROJECT_IDENTITY_ENV_PREFIX}")
string(REPLACE "-" "_" APP_TARGET_PREFIX "${PROJECT_IDENTITY_NAME}")

# ccache (optional): one cache shared by every checkout and worktree (configure it with base_dir = the folder above
# them and hash_dir = false). Used for host builds and, when the toolchain image has it, for the armel build.
option(APP_CCACHE "Compile through ccache when it is installed" ON)
if(APP_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(APP_CCACHE_PROGRAM ccache)
    if(APP_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER ${APP_CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${APP_CCACHE_PROGRAM})
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Which parts to build. The device preset builds the app; the host preset builds the libraries and the tests.
set(APP_DEVICE OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    set(APP_DEVICE ON)
endif()
option(APP_BUILD_TESTS "Build the unit tests (host)" ${PROJECT_IS_TOP_LEVEL})
option(APP_BUILD_DEVICE_APP "Build the device executable (armel)" ${APP_DEVICE})
option(APP_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer (host)" OFF)
option(APP_RT_CHECKS "Trap allocations on real-time threads (debug allocator hook)" OFF)
option(APP_BUILD_NEON_TESTS "Build the NEON kernel tests for the device, run under qemu-arm (scripts/neon-tests.sh)" OFF)
option(APP_BUILD_DEVICE_BENCH "Build the bench executable for the device (scripts/build-armel-bench.sh)" OFF)
option(APP_BUILD_HOST_APP "Build the app executable on the host too (host platform: offline audio, offscreen display)"
       ${PROJECT_IS_TOP_LEVEL})

set(APP_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
configure_file(cmake/ProjectIdentity.h.in ${APP_GENERATED_DIR}/ProjectIdentity.h @ONLY)

include(cmake/TargetFlags.cmake)
include(cmake/Modules.cmake)
include(cmake/Juce.cmake)
include(cmake/Theme.cmake)

# Modules. Every module CMakeLists auto-discovers its sources (cmake/Modules.cmake);
# the dependency rules between them are checked by tools/check-deps.py.
add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/services)
add_subdirectory(src/io)
add_subdirectory(src/ui)
add_subdirectory(src/platform)
if(APP_BUILD_DEVICE_APP OR APP_BUILD_HOST_APP)
    add_subdirectory(src/app)
endif()
if(APP_BUILD_HOST_APP AND NOT APP_BUILD_DEVICE_APP)
    add_subdirectory(tools/render-cli) # <name>_render_cli (host only, tools/render-cli/CMakeLists.txt)
endif()
if(APP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(APP_BUILD_NEON_TESTS)
    add_subdirectory(tests/neon)
endif()
if(APP_BUILD_DEVICE_BENCH)
    add_subdirectory(tests/device-bench) # <name>_bench for the unit (EXCLUDE_FROM_ALL)
endif()
