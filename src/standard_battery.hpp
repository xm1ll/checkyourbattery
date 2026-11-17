#pragma once

#include "hid_io.hpp"

#include <cfgmgr32.h>

struct UsbHidInterface {
    std::wstring path;
    DEVINST node;
};

std::optional<DEVINST> physical_usb_device(DEVINST node);
std::optional<Device> standard_usb_battery(DEVINST usb, std::span<const UsbHidInterface> interfaces, ULONGLONG deadline);
