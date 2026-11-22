# io: device-independent input contracts (io/controllers/ControlSurface.h: logical controls, ControlEvent,
# ControlSource, LampSink, controller profiles) and, later, MIDI parsing. Builds on the host and the device; everything
# that knows a physical device lives in platform/.
#
# Sources: every *.cpp below this folder is compiled into <prefix>_io automatically (cmake/Modules.cmake). No exclusions.
app_add_module(io)
target_link_libraries(${APP_TARGET_PREFIX}_io PUBLIC app::core)
