#pragma once

#include <optional>
#include <string>
#include <vector>

enum class DeviceKind { other, mouse, audio, keyboard, controller };
enum class Transport { bluetooth, receiver };

struct Device {
    std::wstring name;
    DeviceKind kind = DeviceKind::other;
    bool connected = false;
    std::optional<unsigned> percent;
    std::wstring id;
    Transport transport = Transport::bluetooth;
    bool estimated = false;
};

struct ScanResult {
    std::vector<Device> devices;
    bool failed = false;
    bool bluetoothFailed = false;
    bool receiverFailed = false;
};

ScanResult scan_devices();
ScanResult scan_usb_receivers();
ScanResult demo_devices(bool english);
