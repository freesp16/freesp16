# The bench executable for the device: every tests/bench benchmark that makes sense on the reference
# hardware, cross-built in the armel tree (scripts/build-armel-bench.sh → out/armel/<name>_bench) and run on a unit
# over SSH, so every BENCH line carries real Cortex-A9 cycles against its budget (tools/bench-report.py turns
# the output into a table). It is the host bench suite (Catch2, the BENCH() cases and the self-timed cases that call
# bench::record) with the device toolchain and flags; DeviceBenchMain.cpp adds the start-up facts (CPU, governor,
# glibc …), one BENCHCASE line per test case and the crash report.
#
# Sources: tests/bench/**/*.cpp (found at configure time) minus APP_DEVICE_BENCH_EXCLUDE below (host-only benches),
# plus the test fakes. Linking: the module libraries whole-archive except platform (their static registrars: params,
# effects, stretchers, codecs, screens, renderers — like app_link_modules), platform plain by its archive (only the
# registry and the GL binding), JUCE plain. The ui library carries the GL renderers, so — like the app — the GL symbols go through gldispatch stubs bound
# at run time (nothing in DT_NEEDED; a bench never creates a GL renderer). EXCLUDE_FROM_ALL: the device build itself
# (scripts/build-armel.sh) never builds it. APP_ARTIFACT_DIR is a run-time scratch folder (DeviceBenchScratch.h).
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1 # 3.7.1, BSL-1.0 (the same pin as tests/CMakeLists.txt)
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

# Host-only benches (repository-relative): the hidden listening set (writes ~116 WAVs for a listening test on the
# desktop, never a device measurement).
set(APP_DEVICE_BENCH_EXCLUDE
    tests/bench/bench_stretch_render.cpp
    tests/bench/harness/StandaloneMain.cpp)

file(GLOB_RECURSE candidates CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/bench/*.cpp)
set(sources "")
foreach(f IN LISTS candidates)
    file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR} ${f})
    if(NOT rel IN_LIST APP_DEVICE_BENCH_EXCLUDE)
        list(APPEND sources ${f})
    endif()
endforeach()
file(GLOB_RECURSE fakes CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/fakes/*.cpp)
list(LENGTH sources count)
message(STATUS "device bench: ${count} bench sources")

set(target ${APP_TARGET_PREFIX}_device_benches)
add_executable(${target} EXCLUDE_FROM_ALL ${sources} ${fakes} ${CMAKE_CURRENT_SOURCE_DIR}/DeviceBenchMain.cpp
                                          ${CMAKE_SOURCE_DIR}/src/app/Diagnostics.cpp)
target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
# The scratch folder (store benches, bench JSON) is chosen at run time: DeviceBenchScratch.h.
target_compile_definitions(${target} PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}" APP_ARTIFACT_DIR=deviceBenchScratchDir\(\))
target_compile_options(${target} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/DeviceBenchScratch.h)
foreach(lib app::ui app::services app::io app::engine app::core)
    get_target_property(real ${lib} ALIASED_TARGET)
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${real}>")
    set_property(TARGET ${target} PROPERTY LINK_LIBRARY_OVERRIDE_${real} WHOLE_ARCHIVE)
endforeach()
# platform by its archive file: plain, and without its link interface (that would list ui / io / core a second time,
# whole-archive, beside the ones above). Only what the fakes and the GL dispatch need is pulled from it.
target_link_libraries(${target} PRIVATE $<TARGET_FILE:${APP_TARGET_PREFIX}_platform> app::juce app_fonts Catch2::Catch2
                                        ${CMAKE_DL_LIBS})
add_dependencies(${target} ${APP_TARGET_PREFIX}_platform)
app_target_defaults(${target})
set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
                                           OUTPUT_NAME ${PROJECT_IDENTITY_NAME}_bench)

# GL dispatch, as in src/app/CMakeLists.txt (device branch), into this folder's own generated directory.
if(APP_DEVICE)
    set(gldsrc ${CMAKE_SOURCE_DIR}/src/platform/common/display/gldispatch)
    set(glddir ${CMAKE_CURRENT_BINARY_DIR}/gldispatch)
    set(gldlibs $<TARGET_FILE:app_juce> $<TARGET_FILE:${APP_TARGET_PREFIX}_ui> $<TARGET_FILE:${APP_TARGET_PREFIX}_platform>)
    add_custom_command(
        OUTPUT ${glddir}/gldispatch_stubs.S ${glddir}/gldispatch_names.inc
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py ${CMAKE_NM} ${glddir} ${gldlibs}
        DEPENDS app_juce ${APP_TARGET_PREFIX}_ui ${APP_TARGET_PREFIX}_platform ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py
        COMMENT "Generating GL dispatch stubs (device bench)"
        VERBATIM)
    enable_language(ASM)
    # Their own object library: the bench sources' forced include (above) must not reach the assembler.
    add_library(${target}_gldispatch OBJECT EXCLUDE_FROM_ALL ${gldsrc}/GlDispatch.cpp ${glddir}/gldispatch_stubs.S)
    # Only the platform library's include paths and definitions (no link items: those would list the module
    # libraries a second time beside the whole-archive ones above).
    set(platform_lib ${APP_TARGET_PREFIX}_platform)
    target_include_directories(${target}_gldispatch PRIVATE ${CMAKE_SOURCE_DIR}/src ${glddir}
                               $<TARGET_PROPERTY:${platform_lib},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target}_gldispatch PRIVATE
                               $<TARGET_PROPERTY:${platform_lib},INTERFACE_COMPILE_DEFINITIONS>)
    app_target_defaults(${target}_gldispatch)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:${target}_gldispatch>)
endif()
