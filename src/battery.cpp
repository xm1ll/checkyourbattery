#include "battery.hpp"

#include <windows.h>
#include <cfgmgr32.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <tuple>

namespace {
using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace std::chrono_literals;

constexpr wchar_t connectionKey[] = L"System.Devices.Aep.IsConnected";
constexpr wchar_t addressKey[] = L"System.Devices.Aep.DeviceAddress";
constexpr wchar_t containerKey[] = L"System.Devices.Aep.ContainerId";
constexpr wchar_t categoryKey[] = L"System.Devices.Aep.Category";
constexpr wchar_t batteryKey[] = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2";
constexpr wchar_t batteryProperty[] = L"System.Devices.BatteryLife";
constexpr DEVPROPKEY pnpBattery{{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
constexpr DEVPROPKEY shellBattery{{0x49cd1f76, 0x5626, 0x4b17, {0xa4, 0xe8, 0x18, 0xb4, 0xaa, 0x1a, 0x22, 0x13}}, 10};

struct Endpoint {
    Device device;
    hstring endpointId;
    hstring gattId;
    std::wstring address;
    guid container{};
};

std::wstring display_name(std::wstring name) {
    std::erase_if(name, [](wchar_t ch) {
        return ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029 ||
               (ch >= 0x202a && ch <= 0x202e) || (ch >= 0x2066 && ch <= 0x2069);
    });
    if (name.size() > 88) {
        name.resize(87);
        if (IS_HIGH_SURROGATE(name.back())) name.pop_back();
        name += L"…";
    }
    return name.empty() ? L"Устройство Bluetooth" : name;
}

template <typename Operation>
auto wait_for_result(const Operation& operation, std::chrono::seconds timeout) {
    if (const auto status = operation.wait_for(timeout); status == AsyncStatus::Started) {
        operation.Cancel();
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}

IPropertyValue property(const DeviceInformation& info, const hstring& key) {
    const auto properties = info.Properties();
    return properties.HasKey(key) ? properties.Lookup(key).try_as<IPropertyValue>() : nullptr;
}

std::optional<unsigned> battery_property(const IPropertyValue& value) {
    if (!value) return {};
    unsigned level;
    switch (value.Type()) {
    case PropertyType::UInt8: level = value.GetUInt8(); break;
    case PropertyType::UInt16: level = value.GetUInt16(); break;
    case PropertyType::UInt32: level = value.GetUInt32(); break;
    case PropertyType::Int32: level = static_cast<unsigned>(value.GetInt32()); break;
    default: return {};
    }
    if (level <= 100) return level;
    return {};
}

std::wstring normalize_address(std::wstring_view input) {
    std::wstring address;
    for (const wchar_t ch : input) {
        if (ch == L':' || ch == L'-') continue;
        if (!std::iswxdigit(ch)) return {};
        address += static_cast<wchar_t>(std::towupper(ch));
    }
    return address.size() == 12 ? address : std::wstring{};
}

DeviceKind classify(std::wstring text) {
    std::ranges::transform(text, text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const auto has = [&text](std::wstring_view token) { return text.find(token) != std::wstring::npos; };
    if (has(L"mouse") || has(L"мыш")) return DeviceKind::mouse;
    if (has(L"audio") || has(L"headphone") || has(L"headset") || has(L"earbud") ||
        has(L"airpods") || has(L"наушник")) return DeviceKind::audio;
    if (has(L"keyboard") || has(L"клавиатур")) return DeviceKind::keyboard;
    if (has(L"gaming") || has(L"gamepad") || has(L"controller") || has(L"геймпад")) return DeviceKind::controller;
    return DeviceKind::other;
}

std::optional<unsigned> node_battery(const std::wstring& address) {
    if (address.empty()) return {};
    for (const auto prefix : {L"BTHENUM\\DEV_", L"BTHLE\\DEV_"}) {
        auto instance = prefix + address;
        DEVINST instanceHandle{};
        if (CM_Locate_DevNodeW(&instanceHandle, instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        for (const auto* key : {&pnpBattery, &shellBattery}) {
            ULONG type = 0;
            ULONG level = 0;
            ULONG size = sizeof(level);
            if (CM_Get_DevNode_PropertyW(instanceHandle, key, &type,
                    reinterpret_cast<PBYTE>(&level), &size, 0) == CR_SUCCESS &&
                (type == DEVPROP_TYPE_BYTE || type == DEVPROP_TYPE_UINT16 ||
                 type == DEVPROP_TYPE_UINT32 || type == DEVPROP_TYPE_INT32) && level <= 100) return level;
        }
    }
    return {};
}

std::optional<unsigned> gatt_battery(const hstring& endpointId) {
    const auto device = wait_for_result(BluetoothLEDevice::FromIdAsync(endpointId), 4s);
    if (!device) return {};
    const auto services = wait_for_result(device.GetGattServicesForUuidAsync(GattServiceUuids::Battery()), 5s);
    if (services.Status() != GattCommunicationStatus::Success) return {};
    for (const auto& service : services.Services()) {
        const auto chars = wait_for_result(service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel()), 5s);
        if (chars.Status() != GattCommunicationStatus::Success) continue;
        for (const auto& characteristic : chars.Characteristics()) {
            if ((characteristic.CharacteristicProperties() & GattCharacteristicProperties::Read) ==
                GattCharacteristicProperties::None) continue;
            const auto reading = wait_for_result(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached), 5s);
            if (reading.Status() != GattCommunicationStatus::Success) continue;
            if (const auto buffer = reading.Value(); buffer && buffer.Length() == 1) {
                const auto level = DataReader::FromBuffer(buffer).ReadByte();
                if (level <= 100) return level;
            }
        }
    }
    return {};
}

void enumerate(bool lowEnergy, std::vector<Endpoint>& endpoints) {
    const auto selector = lowEnergy ? BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)
                                    : BluetoothDevice::GetDeviceSelectorFromPairingState(true);
    const auto devices = wait_for_result(DeviceInformation::FindAllAsync(selector,
        {connectionKey, addressKey, containerKey, categoryKey, batteryKey, batteryProperty},
        DeviceInformationKind::AssociationEndpoint), 8s);
    for (const auto& info : devices) {
        Endpoint endpoint;
        endpoint.endpointId = info.Id();
        endpoint.device.name = display_name(std::wstring(info.Name()));
        if (const auto value = property(info, connectionKey); value && value.Type() == PropertyType::Boolean)
            endpoint.device.connected = value.GetBoolean();
        if (const auto value = property(info, addressKey); value && value.Type() == PropertyType::String)
            endpoint.address = normalize_address(value.GetString());
        if (const auto value = property(info, containerKey); value && value.Type() == PropertyType::Guid)
            endpoint.container = value.GetGuid();
        if (const auto value = property(info, categoryKey); value && value.Type() == PropertyType::StringArray) {
            com_array<hstring> categories;
            value.GetStringArray(categories);
            for (const auto& category : categories) {
                endpoint.device.kind = classify(std::wstring(category));
                if (endpoint.device.kind != DeviceKind::other) break;
            }
        }
        if (endpoint.device.kind == DeviceKind::other) endpoint.device.kind = classify(endpoint.device.name);
        if (endpoint.device.connected) {
            if (lowEnergy) endpoint.gattId = endpoint.endpointId;
            endpoint.device.percent = battery_property(property(info, batteryKey));
            if (!endpoint.device.percent) endpoint.device.percent = battery_property(property(info, batteryProperty));
            if (!endpoint.device.percent) endpoint.device.percent = node_battery(endpoint.address);
        }
        endpoints.push_back(std::move(endpoint));
    }
}
} // namespace

static ScanResult scan_bluetooth() {
    ScanResult result;
    std::vector<Endpoint> endpoints;
    try {
        winrt::init_apartment();
    } catch (const winrt::hresult_error&) {
        result.failed = true;
        return result;
    }
    struct Apartment {
        ~Apartment() {
            // Each scan tears down its apartment. Release the cached WinRT factories before that.
            winrt::clear_factory_cache();
            winrt::uninit_apartment();
        }
    } apartment;

    int successfulQueries = 0;
    for (const bool lowEnergy : {false, true}) {
        try {
            enumerate(lowEnergy, endpoints);
            ++successfulQueries;
        } catch (const winrt::hresult_error&) {
        }
    }
    result.failed = successfulQueries == 0;

    std::vector<Endpoint> unique;
    for (auto& endpoint : endpoints) {
        const auto match = std::ranges::find_if(unique, [&endpoint](const Endpoint& other) {
            return (!endpoint.address.empty() && endpoint.address == other.address) ||
                   (endpoint.container != winrt::guid{} && endpoint.container == other.container);
        });
        if (match == unique.end()) {
            unique.push_back(std::move(endpoint));
            continue;
        }
        auto& merged = *match;
        if (!merged.device.percent) merged.device.percent = endpoint.device.percent;
        if (merged.device.kind == DeviceKind::other) merged.device.kind = endpoint.device.kind;
        if (merged.device.name == L"Устройство Bluetooth") merged.device.name = endpoint.device.name;
        if (merged.address.empty()) merged.address = std::move(endpoint.address);
        if (merged.container == winrt::guid{}) merged.container = endpoint.container;
        if (!endpoint.gattId.empty()) merged.gattId = std::move(endpoint.gattId);
        merged.device.connected |= endpoint.device.connected;
    }

    for (auto& endpoint : unique) {
        if (!endpoint.device.percent && !endpoint.gattId.empty()) {
            try {
                endpoint.device.percent = gatt_battery(endpoint.gattId);
            } catch (const winrt::hresult_error&) {
            }
        }
        endpoint.device.id = !endpoint.address.empty() ? L"address/" + endpoint.address
                           : endpoint.container != winrt::guid{} ? L"container/" + std::wstring(to_hstring(endpoint.container))
                           : L"endpoint/" + std::wstring(endpoint.endpointId);
        result.devices.push_back(std::move(endpoint.device));
    }
    std::ranges::stable_sort(result.devices, [](const Device& left, const Device& right) {
        return std::tuple(!left.connected, !left.percent.has_value(), left.name) <
               std::tuple(!right.connected, !right.percent.has_value(), right.name);
    });
    return result;
}

ScanResult scan_devices() {
    auto result = scan_usb_receivers();
    result.receiverFailed = result.failed;
    auto bluetooth = scan_bluetooth();
    result.bluetoothFailed = bluetooth.failed;
    for (auto& device : bluetooth.devices) result.devices.push_back(std::move(device));
    result.failed = result.receiverFailed && result.bluetoothFailed;
    return result;
}

ScanResult demo_devices() {
    return {{
        {L"Беспроводная мышь", DeviceKind::mouse, true, 82, L"demo/mouse"},
        {L"Bluetooth-наушники", DeviceKind::audio, true, 18, L"demo/headphones"},
        {L"Беспроводная клавиатура", DeviceKind::keyboard, true, {}, L"demo/keyboard"},
        {L"Игровой контроллер", DeviceKind::controller, false, {}, L"demo/controller"}
    }};
}
