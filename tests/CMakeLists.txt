# Host tests (Catch2 v3, pinned). The device build never compiles tests.
#
# Conventions — every folder is auto-discovered (file(GLOB_RECURSE ... CONFIGURE_DEPENDS)); nobody edits this file to
# add a test, and an empty or missing folder is fine:
#   tests/fakes/**/*.cpp          → library <prefix>_test_fakes, linked into every test executable (fakes of every
#                                   core interface: FakePlatform, FakeAudioDevice, FakeEngineBridge, …)
#   tests/unit/*.cpp              → executable <prefix>_tests, ctest label "unit"
#   tests/unit/<area>/**/*.cpp    → executable <prefix>_tests_<area>, ctest labels "<area>" and "unit"; test names are
#                                   prefixed "<area>: " (e.g. tests/unit/contracts → `ctest -L contracts`)
#   tests/unit/**/test_*.py       → one ctest per script (python3), labels as above; exit code 77 = skipped. The script
#                                   gets APP_SOURCE_DIR, APP_BINARY_DIR and APP_EXECUTABLE (the host app, if built)
#   tests/snapshot/**/*.cpp       → executable <prefix>_snapshot, label "snapshot"
#   tests/golden/                 → its own CMakeLists.txt when present, else **/*.cpp → <prefix>_golden,
#                                   label "golden"
#   tests/bench/**/*.cpp          → executable <prefix>_bench, label "bench"; built only in Release (host-release
#                                   preset: `ctest --preset host-release -L bench`)
# Every executable links the module libraries whole-archive (app_link_modules) so APP_REGISTER_* registrars run, and
# compiles with APP_SOURCE_DIR / APP_ARTIFACT_DIR defined.
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1 # 3.7.1, BSL-1.0
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)
include(Catch)

set(APP_TEST_MODULES app::platform app::ui app::services app::io app::engine app::core)
set(APP_ARTIFACT_DIR ${CMAKE_BINARY_DIR}/artifacts)

file(GLOB_RECURSE APP_FAKE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fakes/*.cpp)
if(APP_FAKE_SOURCES)
    add_library(${APP_TARGET_PREFIX}_test_fakes STATIC ${APP_FAKE_SOURCES})
    target_link_libraries(${APP_TARGET_PREFIX}_test_fakes PUBLIC ${APP_TEST_MODULES} app::juce app_fonts)
    target_include_directories(${APP_TARGET_PREFIX}_test_fakes PUBLIC ${CMAKE_SOURCE_DIR}) # "tests/fakes/..."
    app_target_defaults(${APP_TARGET_PREFIX}_test_fakes)
else()
    add_library(${APP_TARGET_PREFIX}_test_fakes INTERFACE)
endif()

# app_add_test_executable(<target> <labels> <test-name-prefix> <sources...>)
function(app_add_test_executable target labels prefix)
    add_executable(${target} ${ARGN})
    app_link_modules(${target} ${APP_TEST_MODULES})
    target_link_libraries(${target} PRIVATE ${APP_TARGET_PREFIX}_test_fakes Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${target} PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}" APP_ARTIFACT_DIR="${APP_ARTIFACT_DIR}")
    app_target_defaults(${target})
    # Reference expressions must round each operation just as the host engine does, even on CPUs with FMA.
    target_compile_options(${target} PRIVATE $<$<NOT:$<BOOL:${CMAKE_CROSSCOMPILING}>>:-ffp-contract=off>)
    # Tests build descriptors with designated initialisers that name only the members a case needs (the rest keep
    # their defaults on purpose); GCC 15's -Wextra reports every omitted member of those.
    target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
    # Catch tags that are also ctest labels ([fuzz], [soak], [long]: `ctest -L long`, `--label-exclude long`). The
    # pinned Catch2 3.7.1 has no ADD_TAGS_AS_LABELS (3.8+), so each such tag gets its own discovery with a TEST_SPEC
    # (hidden tests such as [.soak] stay hidden; the first matching tag wins, so no test is added twice) and the rest
    # one discovery that excludes them all. catch_discover_tests flattens PROPERTIES, so a label list "a;b" would
    # become LABELS a plus a bogus property: the labels are set by our own test include file instead, which ctest
    # reads after Catch's (it walks each discovery's TEST_LIST).
    set(seen "")
    set(script "")
    foreach(tag fuzz soak long rest)
        if(tag STREQUAL "rest")
            set(spec "${seen}")
            set(list ${target}_plain_TESTS)
            set(tagged ${labels})
        else()
            set(spec "[${tag}]~[.]${seen}")
            set(list ${target}_${tag}_TESTS)
            set(tagged ${labels} ${tag})
            string(APPEND seen "~[${tag}]")
        endif()
        catch_discover_tests(${target} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${prefix}" TEST_SPEC "${spec}"
                             TEST_LIST ${list})
        string(APPEND script "foreach(t IN LISTS ${list})\n  set_tests_properties(\"\${t}\" PROPERTIES LABELS \"${tagged}\")\nendforeach()\n")
    endforeach()
    set(labels_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_labels.cmake")
    file(WRITE "${labels_file}" "${script}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${labels_file}")
endfunction()

# tests/unit: the root folder and one executable per area sub-folder.
file(GLOB APP_UNIT_ROOT CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
if(APP_UNIT_ROOT)
    app_add_test_executable(${APP_TARGET_PREFIX}_tests "unit" "" ${APP_UNIT_ROOT})
endif()
file(GLOB APP_UNIT_AREAS LIST_DIRECTORIES true CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*)
foreach(dir IN LISTS APP_UNIT_AREAS)
    if(NOT IS_DIRECTORY ${dir})
        continue()
    endif()
    get_filename_component(area ${dir} NAME)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${dir}/*.cpp)
    if(sources)
        app_add_test_executable(${APP_TARGET_PREFIX}_tests_${area} "${area};unit" "${area}: " ${sources})
    endif()
endforeach()

# Python tests anywhere below tests/unit.
set(APP_HOST_EXECUTABLE "")
if(TARGET ${PROJECT_IDENTITY_NAME})
    set(APP_HOST_EXECUTABLE $<TARGET_FILE:${PROJECT_IDENTITY_NAME}>)
endif()
file(GLOB_RECURSE APP_PY_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.py)
foreach(script IN LISTS APP_PY_TESTS)
    file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR}/unit ${script})
    get_filename_component(stem ${script} NAME_WE)
    get_filename_component(sub ${rel} DIRECTORY)
    if(sub)
        string(REGEX REPLACE "/.*" "" area ${sub})
        set(labels "${area};unit")
        set(name "${area}: ${stem}")
    else()
        set(labels "unit")
        set(name "${stem}")
    endif()
    add_test(NAME "${name}" COMMAND Python3::Interpreter ${script})
    set_tests_properties("${name}" PROPERTIES LABELS "${labels}" SKIP_RETURN_CODE 77
        ENVIRONMENT "APP_SOURCE_DIR=${CMAKE_SOURCE_DIR};APP_BINARY_DIR=${CMAKE_BINARY_DIR};APP_EXECUTABLE=${APP_HOST_EXECUTABLE}")
endforeach()

# Elapsed-time budgets apply to optimized, uninstrumented playback.
if(NOT APP_SANITIZERS)
    target_compile_definitions(${APP_TARGET_PREFIX}_tests_edit PRIVATE
        $<$<CONFIG:Release,RelWithDebInfo>:APP_CHECK_PLAYBACK_TIMING=1>)
endif()

# Exercise the real dynamic loader with a multi-descriptor bundle on the host architecture.
add_library(clap_effect_fixture MODULE fixtures/clap/EffectFixture.cpp)
target_include_directories(clap_effect_fixture PRIVATE ${CMAKE_SOURCE_DIR}/third_party/clap/include)
set_target_properties(clap_effect_fixture PROPERTIES PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture-plugins")
add_dependencies(${APP_TARGET_PREFIX}_tests_render clap_effect_fixture)
target_compile_definitions(${APP_TARGET_PREFIX}_tests_render PRIVATE CLAP_TEST_EFFECT="$<TARGET_FILE:clap_effect_fixture>")
add_dependencies(${APP_TARGET_PREFIX}_tests_plugin clap_effect_fixture)
target_compile_definitions(${APP_TARGET_PREFIX}_tests_plugin PRIVATE
    CLAP_FIXTURE_DIR="$<TARGET_FILE_DIR:clap_effect_fixture>")

# Snapshot tests.
file(GLOB_RECURSE APP_SNAPSHOT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/snapshot/*.cpp)
if(APP_SNAPSHOT_SOURCES)
    app_add_test_executable(${APP_TARGET_PREFIX}_snapshot "snapshot" "snapshot: " ${APP_SNAPSHOT_SOURCES})
endif()

# Golden tests (tests/golden/CMakeLists.txt when it exists).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/golden/CMakeLists.txt)
    add_subdirectory(golden)
else()
    file(GLOB_RECURSE APP_GOLDEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/golden/*.cpp)
    if(APP_GOLDEN_SOURCES)
        app_add_test_executable(${APP_TARGET_PREFIX}_golden "golden" "golden: " ${APP_GOLDEN_SOURCES})
    endif()
endif()

# Benchmarks: Release builds only; Debug + sanitizer numbers would be meaningless.
file(GLOB_RECURSE APP_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp)
if(APP_BENCH_SOURCES AND CMAKE_BUILD_TYPE STREQUAL "Release")
    app_add_test_executable(${APP_TARGET_PREFIX}_bench "bench" "bench: " ${APP_BENCH_SOURCES})
endif()
