// Common widget vocabulary (Widget.h includes this header for TextMeasure). The component classes live one per
// file next to it (07-ui-design/components/): frame (StatusRow, ContextBar, KnobStrip, ParamColumn), controls (Button,
// Toggle, Segmented, OptionSelector, RailItem, ListRow, Scrollbar, TextField, SoftwareKeyboard) and popups (ListPopup,
// SettingPopup, GridPopup, Modal, Dialog, Message, Progress, Toast). Components know nothing about features: they get
// plain models and ParamSpecs (components/README.md, UID-201).
#pragma once

#include "ui/render/DrawList.h"

#include <functional>
#include <string_view>

namespace ui
{
// Measures text in canvas lpx (backed by the glyph atlas). Widgets need it for mod dots, breadcrumbs and truncation.
using TextMeasure = std::function<float(TextStyleId, std::string_view)>;
} // namespace ui
