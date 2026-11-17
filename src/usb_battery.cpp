#include "battery.hpp"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string_view>

namespace {
struct HandleCloser {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;
using Report = std::array<BYTE, 64>;

struct Receiver {
    USHORT vendor;
    USHORT product;
    std::wstring_view interfacePath;
    USHORT usagePage;
    DeviceKind kind;
    const wchar_t* name;
};

constexpr Receiver maya{0x373e, 0x001e, L"hid#vid_373e&pid_001e&mi_02#", 0xffff, DeviceKind::mouse, L"LAMZU Maya X"};
constexpr Receiver barracuda{0x1532, 0x0552, L"hid#vid_1532&pid_0552&mi_03&col01#", 0xff00, DeviceKind::audio,
                             L"Razer Barracuda X (2022)"};

Handle open_hid(const wchar_t* path, DWORD access) {
    const HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED, nullptr);
    return Handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

bool matches(HANDLE handle, const Receiver& receiver, HIDP_CAPS& caps) {
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(handle, &attributes) || attributes.VendorID != receiver.vendor ||
        attributes.ProductID != receiver.product) return false;
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!HidD_GetPreparsedData(handle, &data)) return false;
    const auto status = HidP_GetCaps(data, &caps);
    HidD_FreePreparsedData(data);
    return status == HIDP_STATUS_SUCCESS && caps.UsagePage == receiver.usagePage && caps.Usage == 0;
}

bool supports_reports(const HIDP_CAPS& caps, bool isMaya) {
    return isMaya ? caps.FeatureReportByteLength == 65
                  : caps.OutputReportByteLength == 64 && caps.InputReportByteLength == 64;
}

std::optional<unsigned> maya_battery(HANDLE handle) {
    // Feature report 0, followed by the Maya X battery query. The leading zero is the Win32 report ID.
    std::array<BYTE, 65> request{0, 0, 0, 2, 2, 0, 0x83};
    if (!HidD_SetFeature(handle, request.data(), static_cast<ULONG>(request.size()))) return {};
    constexpr std::array<BYTE, 6> header{0xa1, 0, 2, 2, 0, 0x83};
    for (int attempt = 0; attempt < 6; ++attempt) {
        Sleep(50);
        std::array<BYTE, 65> reply{};
        if (!HidD_GetFeature(handle, reply.data(), static_cast<ULONG>(reply.size()))) continue;
        const auto payload = std::span(reply).subspan(reply[0] == 0 ? 1 : 0);
        if (std::ranges::equal(header, payload.first(header.size())) && payload[7] <= 100) return payload[7];
    }
    return {};
}

DWORD transfer(HANDLE handle, Report& report, bool write, DWORD waitMs) {
    const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return 0;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD size = 0;
    const auto started = write ? WriteFile(handle, report.data(), static_cast<DWORD>(report.size()), &size, &operation)
                               : ReadFile(handle, report.data(), static_cast<DWORD>(report.size()), &size, &operation);
    if (started) return size;
    if (GetLastError() != ERROR_IO_PENDING) return 0;
    if (WaitForSingleObject(event.get(), waitMs) != WAIT_OBJECT_0) {
        CancelIoEx(handle, &operation);
        GetOverlappedResult(handle, &operation, &size, TRUE);
        return 0;
    }
    return GetOverlappedResult(handle, &operation, &size, FALSE) ? size : 0;
}

template<typename Parse>
auto barracuda_exchange(HANDLE handle, Report request, Parse parse) -> decltype(parse(std::span<const BYTE>{})) {
    if (transfer(handle, request, true, 500) != request.size()) return {};
    const auto deadline = GetTickCount64() + 800;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        Report reply{};
        const auto size = transfer(handle, reply, false, static_cast<DWORD>(deadline - now));
        if (size == 0 || size > reply.size()) break;
        if (auto parsed = parse(std::span<const BYTE>(reply).first(size))) return parsed;
    }
    return {};
}

struct RaceReply {
    BYTE status;
    std::optional<unsigned> millivolts;
};

std::optional<RaceReply> race_reply(std::span<const BYTE> report, BYTE group, BYTE sequence) {
    if (report.size() < 3 || report[0] != 1 || report[1] != 0x80 || report[2] > report.size() - 3) return {};
    auto frames = report.subspan(3, report[2]);
    while (frames.size() >= 10 && frames[0] == 0x50 && frames[1] == 0x49) {
        const auto length = static_cast<std::size_t>(frames[8] | (frames[9] << 8));
        if (length > frames.size() - 10) return {};
        const auto payload = frames.subspan(10, length);
        if (payload.size() >= 3 && payload[0] == group && payload[1] == (0x80 | sequence)) {
            RaceReply result{payload[2], {}};
            if (payload.size() >= 5) result.millivolts = payload[3] | (payload[4] << 8);
            return result;
        }
        frames = frames.subspan(10 + length);
    }
    return {};
}

std::optional<RaceReply> race(HANDLE handle, BYTE group, std::array<BYTE, 3> body) {
    static std::atomic<unsigned> next{0};
    const auto sequence = static_cast<BYTE>(next.fetch_add(1, std::memory_order_relaxed) % 127 + 1);
    return barracuda_exchange(handle, {1, 0x80, 7, 0x50, 0x41, group, sequence, body[0], body[1], body[2]},
                              [=](std::span<const BYTE> reply) { return race_reply(reply, group, sequence); });
}

std::optional<unsigned> barracuda_battery(HANDLE handle) {
    const bool appMode = barracuda_exchange(handle, {1, 0x40}, [](std::span<const BYTE> reply) {
        return reply.size() >= 4 && reply[0] == 1 && reply[1] == 0x40 && reply[2] == 1 && reply[3] == 1;
    });
    if (!appMode) return {};
    const auto remote = race(handle, 0x0e, {0x02, 0xe1, 1});
    std::optional<RaceReply> battery;
    if (remote && remote->status == 0) battery = race(handle, 0x06, {0x01, 0x00, 0x31});
    race(handle, 0x0e, {0x02, 0xe1, 0});
    if (!battery || battery->status != 0 || !battery->millivolts) return {};
    const auto voltage = *battery->millivolts;
    if (voltage < 2500 || voltage > 4500) return {};
    // Approximate discharge curve from the measured Barracuda X (2022) HID implementation.
    constexpr std::array<unsigned, 11> curve{3300, 3680, 3750, 3790, 3830, 3870, 3910, 3960, 4020, 4080, 4150};
    if (voltage <= curve.front()) return 0;
    const auto upper = std::ranges::upper_bound(curve, voltage);
    if (upper == curve.end()) return 100;
    const auto whole = static_cast<unsigned>(upper - curve.begin() - 1) * 10;
    return whole + (voltage - *(upper - 1)) * 10 / (*upper - *(upper - 1));
}

std::optional<Device> read_receiver(const std::wstring& path) {
    auto key = path;
    std::ranges::transform(key, key.begin(), [](wchar_t ch) { return ch >= L'A' && ch <= L'Z' ? ch + 32 : ch; });
    const Receiver* receiver = nullptr;
    for (const auto* candidate : {&maya, &barracuda}) {
        if (key.starts_with(L"\\\\?\\") && std::wstring_view(key).substr(4).starts_with(candidate->interfacePath)) {
            receiver = candidate;
            break;
        }
    }
    if (!receiver) return {};

    const Handle identity = open_hid(path.c_str(), 0);
    HIDP_CAPS caps{};
    if (!identity || !matches(identity.get(), *receiver, caps)) return {};
    const bool isMaya = receiver == &maya;
    if (!supports_reports(caps, isMaya)) return {};

    const auto channel = open_hid(path.c_str(), GENERIC_READ | GENERIC_WRITE);
    HIDP_CAPS channelCaps{};
    const auto percent = channel && matches(channel.get(), *receiver, channelCaps) && supports_reports(channelCaps, isMaya)
        ? (isMaya ? maya_battery(channel.get()) : barracuda_battery(channel.get())) : std::nullopt;
    return Device{receiver->name, receiver->kind, percent.has_value(), percent, L"usb/" + key,
                  Transport::receiver, !isMaya};
}
} // namespace

ScanResult scan_usb_receivers() {
    ScanResult result;
    GUID hid{};
    HidD_GetHidGuid(&hid);
    const HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {{}, true};
    struct DeviceSet { HDEVINFO handle; ~DeviceSet() { SetupDiDestroyDeviceInfoList(handle); } } devices{set};
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid, index, &info)) {
            result.failed = GetLastError() != ERROR_NO_MORE_ITEMS;
            break;
        }
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &info, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &info, detail, required, nullptr, nullptr)) continue;
        if (auto device = read_receiver(detail->DevicePath)) result.devices.push_back(std::move(*device));
    }
    return result;
}
