# engine: real-time code only. May include core/util, core/model/{enums,paths} and core/params/ParamId.h — never
# core/model/Project.h, services, ui or JUCE GUI (module-layout.md dependency rules; tools/check-deps.py).
#
# Sources: every *.cpp below this folder is compiled into <prefix>_engine automatically (cmake/Modules.cmake), with the
# REALTIME flags (-O3, NEON vectorisation, no errno) and tools/rt-lint.py's checks for every file here. No exclusions.
app_add_module(engine REALTIME)
target_link_libraries(${APP_TARGET_PREFIX}_engine PUBLIC app::core)

# IEEE-ordered float on the device for the effects (engine/fx/**): every effect's NEON path is tested bit for bit
# against its scalar reference ("bit-exact on normal inputs"; -funsafe-math-optimizations is not used for code whose
# results are compared bit-exactly). With it, GCC 11 reassociated the scalar IIR references
# in fx/dsp/Filters.cpp, so NEON ≠ scalar under qemu-arm (scripts/neon-tests.sh). Effects vectorise with explicit
# intrinsics, so the auto-vectoriser that needs the flag is not relied on there.
if(CMAKE_CROSSCOMPILING)
    file(GLOB_RECURSE APP_FX_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fx/*.cpp)
    set_source_files_properties(${APP_FX_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-unsafe-math-optimizations")
endif()

# Test-only hooks (e.g. Reverb::overrideConstants for the golden harness) are compiled out of the device app.
if(APP_BUILD_DEVICE_APP)
    target_compile_definitions(${APP_TARGET_PREFIX}_engine PUBLIC APP_TEST_HOOKS=0) # PUBLIC: Reverb.h layout
endif()
