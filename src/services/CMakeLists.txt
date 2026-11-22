# services: non-real-time workers (store, library, render, analysis, interop). May use juce_core, juce_audio_formats,
# juce_data_structures; never the audio thread, never ui or platform (tools/check-deps.py).
#
# Sources: every *.cpp below this folder is compiled into <prefix>_services automatically (cmake/Modules.cmake); an
# empty module still produces a library. No exclusions.
app_add_module(services)
target_link_libraries(${APP_TARGET_PREFIX}_services PUBLIC app::core app::juce)
