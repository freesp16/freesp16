# platform: everything that knows the physical machine — codec, panel, strip, display handles, paths. One
# sub-folder per platform; platform/Platform.h is the contract, PlatformRegistry.cpp the registry
# (APP_REGISTER_PLATFORM). Only app/ and platform/ include platform/** (tools/check-deps.py).
#
# Sources: every *.cpp below this folder is compiled into <prefix>_platform automatically (cmake/Modules.cmake), except:
#   imx6/**                   DEVICE_ONLY  the reference hardware (ALSA, X11, Vivante GC2000 via gldispatch)
#   host/**                   HOST_ONLY    macOS/Linux host: offline audio, offscreen display, no lamps, XDG paths
#   linux/**                  HOST_ONLY + LINUX_ONLY  the Linux desktop: X11 window, ALSA, file audio, Mesa GL
#   common/**                 LINUX_ONLY   shared by imx6 and linux: the X11 window + presenter, direct ALSA, gldispatch
#   common/display/gldispatch/** EXCLUDE   compiled into the app executable (app/CMakeLists.txt): it includes the
#                                          stub list generated from the libraries, this one included
# A new platform (e.g. rpi/) is a new sub-folder; give it its own DEVICE_ONLY/HOST_ONLY line when it needs a sysroot.
app_add_module(platform DEVICE_ONLY imx6 HOST_ONLY host linux LINUX_ONLY common linux EXCLUDE common/display/gldispatch)
find_package(Threads REQUIRED)
target_link_libraries(${APP_TARGET_PREFIX}_platform PUBLIC app::ui app::io app::core Threads::Threads)
if(APP_BUILD_DEVICE_APP)
    find_package(ALSA REQUIRED)
    find_package(X11 REQUIRED)
    target_link_libraries(${APP_TARGET_PREFIX}_platform PUBLIC app::juce ALSA::ALSA X11::X11 ${CMAKE_DL_LIBS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The linux platform: libasound + libX11 (system packages libasound2-dev, libx11-dev). No GL library:
    # the app executable carries the x86-64 gldispatch stubs and loads Mesa at run time.
    find_package(ALSA REQUIRED)
    find_package(X11 REQUIRED)
    target_link_libraries(${APP_TARGET_PREFIX}_platform PUBLIC ALSA::ALSA X11::X11 ${CMAKE_DL_LIBS})
endif()
