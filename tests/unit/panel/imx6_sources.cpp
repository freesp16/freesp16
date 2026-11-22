// platform/imx6/** is DEVICE_ONLY (platform/CMakeLists.txt), but its frame codecs are device-independent: this
// translation unit compiles them into the host test executable so the synthetic-frame tests run on the host.
#include "platform/imx6/panel/AnalogBoard.cpp"
#include "platform/imx6/panel/PanelCodec.cpp"
#include "platform/imx6/panel/PanelLamps.cpp"
#include "platform/imx6/strip/StripCodec.cpp"
