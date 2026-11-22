# core: pure C++20 (no JUCE, no threads of its own). module-layout.md: core/model, commands, edit, params, util.
#
# Sources: every *.cpp below this folder is compiled into <prefix>_core automatically (cmake/Modules.cmake,
# app_add_module); add a file or a sub-folder (e.g. core/params/catalogue/<area>.cpp, core/commands/<area>/) and it
# is built — never edit a list here. No exclusions: core builds identically on the host and the device.
app_add_module(core)
