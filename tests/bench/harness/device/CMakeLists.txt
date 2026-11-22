# Device bench binary (armel) for scripts/emulator-bench.sh: the bench harness (BENCH_STANDALONE: its own main, no
# Catch2) + the chosen tests/bench sources, linked against the module libraries of an existing armel build
# (scripts/build-armel.sh → build/armel), with the same toolchain (toolchain/cross.cmake) and the same flags
# (cmake/TargetFlags.cmake, REALTIME: the engine's -O3/NEON/no-errno set, so inlined kernels compile as in the engine).
# Run inside the toolchain container:
#   cmake -S tests/bench/harness/device -B build/armel-bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/cross.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DAPP_BENCH_SOURCES="tests/bench/bench_harness.cpp"
cmake_minimum_required(VERSION 3.25)
get_filename_component(APP_REPO ${CMAKE_CURRENT_LIST_DIR}/../../../.. ABSOLUTE)
include(${APP_REPO}/cmake/ProjectIdentity.cmake)
project(${PROJECT_IDENTITY_NAME}_device_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
string(REPLACE "-" "_" APP_TARGET_PREFIX "${PROJECT_IDENTITY_NAME}")
set(APP_ARMEL_BUILD ${APP_REPO}/build/armel CACHE PATH "the armel build whose module libraries the bench links")
set(APP_BENCH_SOURCES "" CACHE STRING "bench sources, repository-relative, ';'-separated")
set(APP_GENERATED_DIR ${APP_ARMEL_BUILD}/generated)
if(NOT EXISTS ${APP_GENERATED_DIR}/ProjectIdentity.h)
    message(FATAL_ERROR "no armel build at ${APP_ARMEL_BUILD}: run scripts/build-armel.sh first")
endif()
include(${APP_REPO}/cmake/TargetFlags.cmake)

set(sources ${APP_BENCH_SOURCES})
list(TRANSFORM sources PREPEND ${APP_REPO}/)
set(bench ${APP_TARGET_PREFIX}_device_bench)
add_executable(${bench} ${APP_REPO}/tests/bench/harness/Bench.cpp ${APP_REPO}/tests/bench/harness/StandaloneMain.cpp
                        ${sources})
target_compile_definitions(${bench} PRIVATE BENCH_STANDALONE=1 APP_SOURCE_DIR="${APP_REPO}")
target_include_directories(${bench} PRIVATE ${APP_REPO} ${APP_REPO}/src)
app_target_defaults(${bench} REALTIME)

# engine + core whole-archive (their static registrars, like app_link_modules); the rest only as needed.
set(whole "")
foreach(module engine core)
    set(lib ${APP_ARMEL_BUILD}/src/${module}/lib${APP_TARGET_PREFIX}_${module}.a)
    if(EXISTS ${lib})
        list(APPEND whole ${lib})
    endif()
endforeach()
set(rest "")
foreach(lib src/io/lib${APP_TARGET_PREFIX}_io.a src/services/lib${APP_TARGET_PREFIX}_services.a src/ui/lib${APP_TARGET_PREFIX}_ui.a
            src/platform/lib${APP_TARGET_PREFIX}_platform.a libapp_juce.a libapp_fonts.a)
    if(EXISTS ${APP_ARMEL_BUILD}/${lib})
        list(APPEND rest ${APP_ARMEL_BUILD}/${lib})
    endif()
endforeach()
target_link_libraries(${bench} PRIVATE -Wl,--whole-archive ${whole} -Wl,--no-whole-archive -Wl,--start-group ${rest}
                                       ${whole} -Wl,--end-group pthread dl rt m)
set_target_properties(${bench} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
