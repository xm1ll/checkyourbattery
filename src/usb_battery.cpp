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

enum class Protocol { maya, barracuda, arctisNova7, arctisNova7Steps, arctisNova5, arctis7Plus,
                      arctisGameBuds, aerox, hyperxCloud, nzxt };

struct Receiver {
    USHORT vendor;
    USHORT product;
    std::wstring_view interfacePath;
    USHORT usagePage;
    USHORT usage;
    Protocol protocol;
    DeviceKind kind;
    const wchar_t* name;
};

constexpr Receiver receivers[]{
    {0x373e, 0x001e, L"hid#vid_373e&pid_001e&mi_02#", 0xffff, 0, Protocol::maya, DeviceKind::mouse, L"LAMZU Maya X"},
    {0x1532, 0x0552, L"hid#vid_1532&pid_0552&mi_03&col01#", 0xff00, 0, Protocol::barracuda,
     DeviceKind::audio, L"Razer Barracuda X (2022)"},
    {0x1038, 0x22a1, L"hid#vid_1038&pid_22a1&mi_03", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7"},
    {0x1038, 0x227e, L"hid#vid_1038&pid_227e&mi_03", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7 Gen 2"},
    {0x1038, 0x1838, L"hid#vid_1038&pid_1838&mi_03", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 3 Wireless"},
    {0x1038, 0x1852, L"hid#vid_1038&pid_1852&mi_03", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 5 Wireless"},
    {0x1038, 0x1858, L"hid#vid_1038&pid_1858&mi_03", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 9 Wireless"},
    {0x1038, 0x2202, L"hid#vid_1038&pid_2202&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7"},
    {0x1038, 0x2206, L"hid#vid_1038&pid_2206&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x2258, L"hid#vid_1038&pid_2258&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x229e, L"hid#vid_1038&pid_229e&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x22ad, L"hid#vid_1038&pid_22ad&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x22a4, L"hid#vid_1038&pid_22a4&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x22a5, L"hid#vid_1038&pid_22a5&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7X"},
    {0x1038, 0x220a, L"hid#vid_1038&pid_220a&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7P"},
    {0x1038, 0x22a7, L"hid#vid_1038&pid_22a7&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7P"},
    {0x1038, 0x2298, L"hid#vid_1038&pid_2298&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7P"},
    {0x1038, 0x223a, L"hid#vid_1038&pid_223a&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7 Diablo IV Edition"},
    {0x1038, 0x22a9, L"hid#vid_1038&pid_22a9&", 0xffc0, 1, Protocol::arctisNova7,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7 Diablo IV Edition"},
    {0x1038, 0x227a, L"hid#vid_1038&pid_227a&", 0xffc0, 1, Protocol::arctisNova7Steps,
     DeviceKind::audio, L"SteelSeries Arctis Nova 7 WoW Edition"},
    {0x1038, 0x2232, L"hid#vid_1038&pid_2232&", 0xffc0, 1, Protocol::arctisNova5,
     DeviceKind::audio, L"SteelSeries Arctis Nova 5"},
    {0x1038, 0x2253, L"hid#vid_1038&pid_2253&", 0xffc0, 1, Protocol::arctisNova5,
     DeviceKind::audio, L"SteelSeries Arctis Nova 5X"},
    {0x1038, 0x2269, L"hid#vid_1038&pid_2269&", 0xffc0, 1, Protocol::arctisNova5,
     DeviceKind::audio, L"SteelSeries Arctis Nova 3P Wireless"},
    {0x1038, 0x226d, L"hid#vid_1038&pid_226d&", 0xffc0, 1, Protocol::arctisNova5,
     DeviceKind::audio, L"SteelSeries Arctis Nova 3X Wireless"},
    {0x1038, 0x220e, L"hid#vid_1038&pid_220e&", 0xffc0, 1, Protocol::arctis7Plus,
     DeviceKind::audio, L"SteelSeries Arctis 7+"},
    {0x1038, 0x2212, L"hid#vid_1038&pid_2212&", 0xffc0, 1, Protocol::arctis7Plus,
     DeviceKind::audio, L"SteelSeries Arctis 7+ PlayStation"},
    {0x1038, 0x2216, L"hid#vid_1038&pid_2216&", 0xffc0, 1, Protocol::arctis7Plus,
     DeviceKind::audio, L"SteelSeries Arctis 7+ Xbox"},
    {0x1038, 0x2236, L"hid#vid_1038&pid_2236&", 0xffc0, 1, Protocol::arctis7Plus,
     DeviceKind::audio, L"SteelSeries Arctis 7+ Destiny Edition"},
    {0x1038, 0x230a, L"hid#vid_1038&pid_230a&", 0xffc0, 1, Protocol::arctisGameBuds,
     DeviceKind::audio, L"SteelSeries Arctis GameBuds"},
    {0x1038, 0x2317, L"hid#vid_1038&pid_2317&", 0xffc0, 1, Protocol::arctisGameBuds,
     DeviceKind::audio, L"SteelSeries Arctis GameBuds X"},
    {0x1038, 0x1878, L"hid#vid_1038&pid_1878&", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 3 Wireless CS2 Dragon Lore"},
    {0x1038, 0x185c, L"hid#vid_1038&pid_185c&", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 5 Wireless Destiny 2 Edition"},
    {0x1038, 0x1860, L"hid#vid_1038&pid_1860&", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 5 Wireless Diablo IV Edition"},
    {0x1038, 0x1874, L"hid#vid_1038&pid_1874&", 0xffc0, 1, Protocol::aerox,
     DeviceKind::mouse, L"SteelSeries Aerox 9 Wireless WoW Edition"},
    {0x03f0, 0x05b7, L"hid#vid_03f0&pid_05b7&", 0xff13, 1, Protocol::hyperxCloud,
     DeviceKind::audio, L"HyperX Cloud III Wireless"},
    {0x03f0, 0x0995, L"hid#vid_03f0&pid_0995&", 0xff13, 1, Protocol::hyperxCloud,
     DeviceKind::audio, L"HyperX Cloud II Core Wireless"},
    {0x1e71, 0x2101, L"hid#vid_1e71&pid_2101&", 0xffca, 1, Protocol::nzxt,
     DeviceKind::mouse, L"NZXT Lift Elite Wireless"},
};

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
    return status == HIDP_STATUS_SUCCESS && caps.UsagePage == receiver.usagePage && caps.Usage == receiver.usage;
}

bool supports_reports(const HIDP_CAPS& caps, Protocol protocol) {
    if (protocol == Protocol::maya) return caps.FeatureReportByteLength == 65;
    if (protocol == Protocol::barracuda) return caps.OutputReportByteLength == 64 && caps.InputReportByteLength == 64;
    if (protocol == Protocol::hyperxCloud) {
        return (caps.OutputReportByteLength == 62 || caps.FeatureReportByteLength == 62) &&
               caps.InputReportByteLength >= 62 && caps.InputReportByteLength <= 65;
    }
    if (protocol == Protocol::nzxt) return caps.OutputReportByteLength == 64 && caps.InputReportByteLength == 64;
    // Windows may include a synthetic zero report ID in the collection's lengths.
    return (caps.OutputReportByteLength == 64 || caps.OutputReportByteLength == 65) &&
           (caps.InputReportByteLength == 64 || caps.InputReportByteLength == 65);
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

DWORD transfer(HANDLE handle, std::span<BYTE> report, bool write, DWORD waitMs) {
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
auto read_reply(HANDLE handle, std::span<BYTE> reply, Parse parse)
    -> decltype(parse(std::span<const BYTE>{})) {
    const auto deadline = GetTickCount64() + 800;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        const auto size = transfer(handle, reply, false, static_cast<DWORD>(deadline - now));
        if (size == 0 || size > reply.size()) break;
        if (auto parsed = parse(std::span<const BYTE>(reply).first(size))) return parsed;
    }
    return {};
}

template<typename Parse>
auto exchange(HANDLE handle, std::span<BYTE> request, std::span<BYTE> reply, Parse parse)
    -> decltype(parse(std::span<const BYTE>{})) {
    if (transfer(handle, request, true, 500) != request.size()) return {};
    return read_reply(handle, reply, parse);
}

template<typename Parse>
auto barracuda_exchange(HANDLE handle, Report request, Parse parse) {
    Report reply{};
    return exchange(handle, request, reply, parse);
}

std::optional<unsigned> steelseries_reply(std::span<const BYTE> report, Protocol protocol) {
    if (!report.empty() && report.front() == 0) report = report.subspan(1);
    if (protocol == Protocol::aerox) {
        if (report.size() < 2 || report[0] != 0xd2) return {};
        const unsigned level = report[1] & 0x7f;  // High bit indicates charging.
        if (level >= 1 && level <= 21) return (level - 1) * 5;
        if (level > 21 && level <= 100) return level;
        return {};
    }
    if (report.size() < 4 || report[0] != 0xb0) return {};
    if (protocol == Protocol::arctisNova7 || protocol == Protocol::arctisNova7Steps) {
        // The receiver can retain the last percentage after the headset disconnects.
        if (report[1] != 3 || report[3] == 0) return {};
        if (protocol == Protocol::arctisNova7Steps) {
            if (report[2] <= 4) return report[2] * 25;
        } else if (report[2] <= 100) {
            return report[2];
        }
    } else if (protocol == Protocol::arctisNova5) {
        if (report.size() >= 5 && report[1] != 2 && report[3] <= 100) return report[3];
    } else if (protocol == Protocol::arctis7Plus) {
        if (report[1] != 1 && report[2] <= 4) return report[2] * 25;
    } else if (protocol == Protocol::arctisGameBuds && report.size() >= 7) {
        std::optional<unsigned> level;
        for (std::size_t bud = 0; bud < 2; ++bud) {
            if (report[3 + bud] == 3 && report[5 + bud] <= 100) {
                level = std::min(level.value_or(100), static_cast<unsigned>(report[5 + bud]));
            }
        }
        return level;
    }
    return {};
}

std::optional<unsigned> steelseries_battery(HANDLE handle, const HIDP_CAPS& caps, Protocol protocol) {
    const BYTE query = protocol == Protocol::aerox ? 0xd2 : 0xb0;
    std::array<BYTE, 65> request{0, query};
    std::array<BYTE, 65> reply{};
    return exchange(handle, std::span(request).first(caps.OutputReportByteLength),
                    std::span(reply).first(caps.InputReportByteLength),
                    [protocol](std::span<const BYTE> report) { return steelseries_reply(report, protocol); });
}

std::optional<unsigned> hyperx_reply(std::span<const BYTE> report) {
    if (report.size() >= 5 && report[0] == 0x66 && (report[1] == 0x89 || report[1] == 0x0d) &&
        (report[2] != 0 || report[3] != 0) && report[4] <= 100) return report[4];
    return {};
}

std::optional<unsigned> hyperx_battery(HANDLE handle, const HIDP_CAPS& caps) {
    std::array<BYTE, 62> request{0x66, 0x89};
    std::array<BYTE, 65> reply{};
    const bool sent = caps.OutputReportByteLength == request.size() &&
                      transfer(handle, request, true, 500) == request.size();
    if (!sent && (caps.FeatureReportByteLength != request.size() ||
                  !HidD_SetFeature(handle, request.data(), static_cast<ULONG>(request.size())))) return {};
    return read_reply(handle, std::span(reply).first(caps.InputReportByteLength), hyperx_reply);
}

std::optional<unsigned> nzxt_reply(std::span<const BYTE> report) {
    if (report.size() >= 27 && report[0] == 0x4e && report[1] == 2 && report[2] == 0x97 &&
        report[18] == 0 && report[17] <= 100) return report[17];
    return {};
}

std::optional<unsigned> nzxt_battery(HANDLE handle) {
    Report request{0x4e, 2, 0x81, 0, 0xb0};
    Report reply{};
    return exchange(handle, request, reply, nzxt_reply);
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
    if (!key.starts_with(L"\\\\?\\")) return {};
    const auto interfacePath = std::wstring_view(key).substr(4);
    const auto receiver = std::ranges::find_if(receivers, [interfacePath](const Receiver& candidate) {
        if (!interfacePath.starts_with(candidate.interfacePath)) return false;
        if (candidate.interfacePath.ends_with(L'#') || candidate.interfacePath.ends_with(L'&')) return true;
        const auto suffix = interfacePath.substr(candidate.interfacePath.size());
        return suffix.starts_with(L'#') || suffix.starts_with(L"&col");
    });
    if (receiver == std::end(receivers)) return {};

    const Handle identity = open_hid(path.c_str(), 0);
    HIDP_CAPS caps{};
    if (!identity || !matches(identity.get(), *receiver, caps) || !supports_reports(caps, receiver->protocol)) return {};

    const auto channel = open_hid(path.c_str(), GENERIC_READ | GENERIC_WRITE);
    HIDP_CAPS channelCaps{};
    std::optional<unsigned> percent;
    if (channel && matches(channel.get(), *receiver, channelCaps) && supports_reports(channelCaps, receiver->protocol)) {
        switch (receiver->protocol) {
        case Protocol::maya: percent = maya_battery(channel.get()); break;
        case Protocol::barracuda: percent = barracuda_battery(channel.get()); break;
        case Protocol::hyperxCloud: percent = hyperx_battery(channel.get(), channelCaps); break;
        case Protocol::nzxt: percent = nzxt_battery(channel.get()); break;
        default: percent = steelseries_battery(channel.get(), channelCaps, receiver->protocol); break;
        }
    }
    return Device{receiver->name, receiver->kind, percent.has_value(), percent, L"usb/" + key,
                  Transport::receiver, receiver->protocol == Protocol::barracuda};
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
