# Compiler flags. The device is a Cortex-A9 (ARMv7-A, NEON, 32 KB L1 I/D, ~1 GHz, softfp ABI); performance
# comes first. The arch flags themselves come from toolchain/cross.cmake.
#
#   app_target_defaults(<target> [REALTIME])
#     every target: warnings, section GC, -O3 in Release and RelWithDebInfo (every own target and the JUCE
#                   static library; never -Os — I-cache pressure is handled by Thumb-2 and section GC)
#     REALTIME:     DSP flags for the engine (float vectorisation on NEON, no errno, FMA contraction) and the
#                   real-time lint/allocator hooks (tools/rt-lint.py scans every REALTIME module: engine/**)
#
#   app_release_o3(<target>)  only the -O3 rule, for third-party targets we compile (app_juce, cmake/Juce.cmake)

set(APP_WARNINGS -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual -Wdouble-promotion
                 -Wimplicit-fallthrough)

function(app_release_o3 target)
    # Later flags win: this overrides CMake's RelWithDebInfo default (-O2) and any -O level a toolchain file adds.
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
endfunction()

function(app_target_defaults target)
    cmake_parse_arguments(ARG "REALTIME" "" "" ${ARGN})
    target_compile_options(${target} PRIVATE ${APP_WARNINGS})
    app_release_o3(${target})
    target_include_directories(${target} PUBLIC ${APP_GENERATED_DIR})
    if(CMAKE_CROSSCOMPILING)
        # Smaller binary (I-cache, boot archive budget): drop unused functions and data at link time.
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    endif()
    if(ARG_REALTIME)
        # NEON is not IEEE-754 compliant (flush-to-zero), so GCC only vectorises float loops for it with
        # -funsafe-math-optimizations. Goldens compare with a tolerance (tech-stack.md numerics policy), so this is
        # allowed in the engine; it is NOT used for code whose results are compared bit-exactly.
        # FMA contraction: the Cortex-A9 (VFPv3, ARMv7 NEON) has no fused multiply-add, so -ffp-contract=fast is a no-op
        # there; on the host (Apple silicon has FMA) contraction is OFF so host numerics match the device and the
        # bit-exact parity goldens hold in every build type.
        target_compile_options(${target} PRIVATE
            -fno-math-errno -fno-trapping-math
            $<IF:$<BOOL:${CMAKE_CROSSCOMPILING}>,-ffp-contract=fast,-ffp-contract=off>
            $<$<BOOL:${CMAKE_CROSSCOMPILING}>:-funsafe-math-optimizations>)
        target_compile_definitions(${target} PRIVATE APP_REALTIME_TU=1)
    endif()
    if(APP_SANITIZERS AND NOT CMAKE_CROSSCOMPILING)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
    if(APP_RT_CHECKS)
        target_compile_definitions(${target} PUBLIC APP_RT_CHECKS=1)
    endif()
endfunction()

if(CMAKE_CROSSCOMPILING)
    add_link_options(-Wl,--gc-sections -Wl,-O1)
endif()
