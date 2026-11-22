# NEON kernel tests for the device (armel), run under qemu-arm in the toolchain container (scripts/neon-tests.sh).
#
# The host suite checks every NEON kernel against its scalar reference only where the host has NEON (Apple silicon);
# on an x86-64 desktop those tests SKIP. This target compiles the same test sources with the device toolchain and
# flags (Cortex-A9, NEON, softfp) and runs them in user-mode qemu (QEMU_CPU=cortex-a9), so NEON ↔ scalar parity is
# checked on every development machine. qemu says nothing about A9 cycles: budgets stay a hardware measurement.
#
# Sources: every unit test file that has a NEON branch (it mentions __ARM_NEON or a *_HAVE_NEON / *_NEON switch),
# found at configure time, so a new kernel test is picked up without editing this file.
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1 # 3.7.1, BSL-1.0 (the same pin as tests/CMakeLists.txt)
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

file(GLOB_RECURSE candidates CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/*.cpp)
set(APP_NEON_TEST_SOURCES "")
foreach(f IN LISTS candidates)
    file(STRINGS ${f} hits REGEX "__ARM_NEON|_HAVE_NEON|APP_RENDER_NEON")
    if(hits)
        list(APPEND APP_NEON_TEST_SOURCES ${f})
    endif()
endforeach()
list(LENGTH APP_NEON_TEST_SOURCES count)
message(STATUS "NEON tests: ${count} test files with a NEON branch")

set(target ${APP_TARGET_PREFIX}_neon_tests)
add_executable(${target} EXCLUDE_FROM_ALL ${APP_NEON_TEST_SOURCES})
# A plain link (not whole-archive): the kernel tests need no registrars, and the device GL renderer must stay out.
target_link_libraries(${target} PRIVATE app::ui app::services app::engine app::core) # services: analysis kernels (SliceDetect, peaks)
target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/unit/voice
                                             ${CMAKE_SOURCE_DIR}/tests/unit/mixer)
target_compile_definitions(${target} PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                             APP_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/artifacts")
app_target_defaults(${target})
set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
