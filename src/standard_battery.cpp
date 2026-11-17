#include "standard_battery.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {
constexpr USAGE genericDeviceControls = 0x06;
constexpr USAGE batteryStrength = 0x20;
constexpr unsigned maxPnPDepth = 16;

std::wstring node_id(DEVINST node) {
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(node, id, ARRAYSIZE(id), 0) != CR_SUCCESS) return {};
    std::wstring result(id);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) { return ch >= L'A' && ch <= L'Z' ? ch + 32 : ch; });
    return result;
}

std::wstring usb_name(DEVINST node) {
    constexpr DEVPROPKEY friendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    constexpr DEVPROPKEY busName{{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};
    for (const auto* key : {&friendlyName, &busName}) {
        wchar_t name[256]{};
        DEVPROPTYPE type = 0;
        ULONG bytes = sizeof(name);
        if (CM_Get_DevNode_PropertyW(node, key, &type, reinterpret_cast<PBYTE>(name), &bytes, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_STRING && bytes >= sizeof(wchar_t) && bytes <= sizeof(name) &&
            bytes % sizeof(wchar_t) == 0 && name[bytes / sizeof(wchar_t) - 1] == L'\0') {
            if (auto label = display_name(name); !label.empty()) return label;
        }
    }
    return {};
}

std::optional<unsigned> device_property(DEVINST usb, std::span<const UsbHidInterface> interfaces) {
    if (auto level = windows_device_battery(usb)) return level;
    for (const auto& interface : interfaces) {
        auto node = interface.node;
        for (unsigned depth = 0; node != usb && depth < maxPnPDepth; ++depth) {
            if (auto level = windows_device_battery(node)) return level;
            DEVINST parent{};
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
            node = parent;
        }
    }
    return {};
}

std::optional<unsigned> strength_percent(ULONG raw, const HIDP_VALUE_CAPS& value) {
    if (!value.IsAbsolute || value.BitSize == 0 || value.BitSize > 32 || value.LogicalMin >= value.LogicalMax)
        return {};
    const auto modulus = std::int64_t{1} << value.BitSize;
    if (raw >= modulus) return {};
    std::int64_t level = raw;
    if (value.LogicalMin < 0 && level >= modulus / 2) level -= modulus;
    // A null value is outside the logical range. Do not turn it into an empty or full battery.
    if (level < value.LogicalMin || level > value.LogicalMax) return {};
    const auto range = std::int64_t{value.LogicalMax} - value.LogicalMin;
    return static_cast<unsigned>(((level - value.LogicalMin) * 100 + range / 2) / range);
}

DeviceKind kind_from_hid(const HIDP_CAPS& caps) {
    if (caps.UsagePage != 1) return DeviceKind::other;  // Generic Desktop.
    switch (caps.Usage) {
    case 2: return DeviceKind::mouse;
    case 4: case 5: return DeviceKind::controller;
    case 6: return DeviceKind::keyboard;
    default: return DeviceKind::other;
    }
}

std::optional<unsigned> hid_battery_strength(const std::wstring& path, DeviceKind& kind, ULONGLONG deadline) {
    // Use a shared handle with no input or output access. Only GET_FEATURE is used.
    const auto handle = hid::open_hid(path.c_str(), 0);
    if (!handle) return {};
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!HidD_GetPreparsedData(handle.get(), &data)) return {};
    const std::unique_ptr<std::remove_pointer_t<PHIDP_PREPARSED_DATA>, decltype(&HidD_FreePreparsedData)>
        descriptor(data, &HidD_FreePreparsedData);
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data, &caps) != HIDP_STATUS_SUCCESS) return {};
    if (kind == DeviceKind::other) kind = kind_from_hid(caps);
    if (caps.FeatureReportByteLength < 2 || caps.FeatureReportByteLength > 1024 || !caps.NumberFeatureValueCaps)
        return {};

    HIDP_VALUE_CAPS strength{};
    USHORT count = 1;
    if (HidP_GetSpecificValueCaps(HidP_Feature, genericDeviceControls, 0, batteryStrength, &strength, &count, data)
            != HIDP_STATUS_SUCCESS || count != 1 || strength.ReportCount != 1 ||
        !strength.IsAbsolute || strength.BitSize == 0 || strength.BitSize > 32 ||
        strength.LogicalMin >= strength.LogicalMax) return {};

    const auto now = GetTickCount64();
    if (now >= deadline) return {};
    std::vector<BYTE> report(caps.FeatureReportByteLength);
    report[0] = strength.ReportID;
    const auto bytes = hid::get_feature(handle.get(), report, static_cast<DWORD>(std::min<ULONGLONG>(150, deadline - now)));
    // Windows can omit the dummy zero report ID from the returned byte count.
    if (bytes == 0 || (bytes != report.size() && !(strength.ReportID == 0 && bytes + 1 == report.size())) ||
        report[0] != strength.ReportID) return {};
    ULONG raw = 0;
    if (HidP_GetUsageValue(HidP_Feature, genericDeviceControls, strength.LinkCollection, batteryStrength, &raw, data,
                           reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS)
        return {};
    return strength_percent(raw, strength);
}
} // namespace

std::optional<DEVINST> physical_usb_device(DEVINST node) {
    for (unsigned depth = 0; depth < maxPnPDepth; ++depth) {
        const auto id = node_id(node);
        if (id.empty() || id.starts_with(L"bth") || id.starts_with(L"bluetooth\\")) return {};
        if (id.starts_with(L"usb\\vid_")) {
            const auto end = id.find(L'\\', 4);
            if (id.substr(4, end == std::wstring::npos ? end : end - 4).find(L"&mi_") == std::wstring::npos)
                return node;
        }
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return {};
}

std::optional<Device> standard_usb_battery(DEVINST usb, std::span<const UsbHidInterface> interfaces, ULONGLONG deadline) {
    auto level = device_property(usb, interfaces);
    auto kind = DeviceKind::other;
    for (const auto& interface : interfaces) {
        if (level || GetTickCount64() >= deadline) break;
        level = hid_battery_strength(interface.path, kind, deadline);
    }
    if (!level) return {};
    auto id = node_id(usb);
    if (id.empty()) return {};
    auto name = usb_name(usb);
    if (kind == DeviceKind::other) kind = classify_device_name(name);
    return Device{std::move(name), kind, true, level, L"usb/discovered/" + id, Transport::usb};
}
