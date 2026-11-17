#pragma once

#include "battery.hpp"
#include <windows.h>

HICON create_tray_icon(DeviceKind kind, std::optional<unsigned> percent, bool showPercent = false);
