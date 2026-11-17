#include "hid_io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>

namespace {
using namespace hid;

constexpr USHORT logitechVendor = 0x046d;
constexpr USHORT logitechReceivers[]{
    0xc518, 0xc51a, 0xc51b, 0xc521, 0xc525, 0xc526, 0xc52b, 0xc52e, 0xc52f, 0xc531, 0xc532, 0xc534,
    0xc535, 0xc537, 0xc539, 0xc53a, 0xc53d, 0xc53f, 0xc541, 0xc545, 0xc547, 0xc548, 0xc54d,
};
struct LogitechHeadset { USHORT pid; const wchar_t* name; };
constexpr LogitechHeadset logitechHeadsets[]{
    {0x0a66, L"Logitech G533"}, {0x0a5b, L"Logitech G933"}, {0x0a87, L"Logitech G935"},
    {0x0ab5, L"Logitech G733"}, {0x0afe, L"Logitech G733"}, {0x0b1f, L"Logitech G733"},
    {0x0ac4, L"Logitech G535"},
};

std::optional<USHORT> logitech_pid(std::wstring_view path) {
    constexpr std::wstring_view prefix = L"\\\\?\\hid#vid_046d&pid_";
    if (!path.starts_with(prefix) || path.size() <= prefix.size() + 4) return {};
    const auto suffix = path.substr(prefix.size());
    if (suffix[4] != L'&' && suffix[4] != L'#') return {};
    unsigned pid = 0;
    for (const auto ch : suffix.substr(0, 4)) {
        if (ch >= L'0' && ch <= L'9') pid = 16 * pid + ch - L'0';
        else if (ch >= L'a' && ch <= L'f') pid = 16 * pid + ch - L'a' + 10;
        else return {};
    }
    return static_cast<USHORT>(pid);
}

// The short and long top-level collections share an interface and PnP instance stem.
std::wstring logitech_interface_key(std::wstring_view path) {
    constexpr std::size_t hardwareStart = 8;  // After "\\?\hid#".
    const auto hardwareEnd = path.find(L'#', hardwareStart);
    if (hardwareEnd == std::wstring_view::npos) return {};
    const auto instanceEnd = path.find(L'#', hardwareEnd + 1);
    if (instanceEnd == std::wstring_view::npos) return {};
    auto hardware = path.substr(0, hardwareEnd);
    auto instance = path.substr(hardwareEnd, instanceEnd - hardwareEnd);
    const auto collection = hardware.rfind(L"&col");
    const auto collectionInstance = instance.rfind(L'&');
    if (collection != std::wstring_view::npos && hardware.size() - collection == 6 &&
        collectionInstance != std::wstring_view::npos && instance.size() - collectionInstance == 5) {
        hardware = hardware.substr(0, collection);
        instance = instance.substr(0, collectionInstance);
    }
    return std::wstring(hardware) + std::wstring(instance);
}

enum class LogitechChannel { invalid, shortReport, longReport, headset };

LogitechChannel logitech_channel(const HIDP_CAPS& caps, bool headset) {
    if (caps.UsagePage == 0xff00 && caps.Usage == 1 &&
        caps.InputReportByteLength == 7 && caps.OutputReportByteLength == 7) return LogitechChannel::shortReport;
    if (caps.InputReportByteLength != 20 || caps.OutputReportByteLength != 20) return LogitechChannel::invalid;
    if (caps.UsagePage == 0xff00 && caps.Usage == 2) return LogitechChannel::longReport;
    if (headset && caps.UsagePage == 0xff43 && caps.Usage == 0x0202) return LogitechChannel::headset;
    return LogitechChannel::invalid;
}

struct LogitechReceiver {
    USHORT pid;
    const wchar_t* headset;
    std::wstring shortPath;
    std::wstring longPath;
    std::wstring headsetPath;
};

void collect_logitech(std::map<std::wstring, LogitechReceiver>& receivers, const std::wstring& path,
                      const std::wstring& key, USHORT pid) {
    const auto headset = std::ranges::find(logitechHeadsets, pid, &LogitechHeadset::pid);
    const bool isHeadset = headset != std::end(logitechHeadsets);
    if (!isHeadset && std::ranges::find(logitechReceivers, pid) == std::end(logitechReceivers)) return;

    const auto identity = open_hid(path.c_str(), 0);
    HIDP_CAPS caps{};
    if (!identity || !hid_caps(identity.get(), logitechVendor, pid, caps)) return;
    const auto channel = logitech_channel(caps, isHeadset);
    if (channel == LogitechChannel::invalid) return;
    const auto group = logitech_interface_key(key);
    if (group.empty()) return;
    auto& receiver = receivers.try_emplace(group, LogitechReceiver{pid, isHeadset ? headset->name : nullptr, {}, {}, {}}).first->second;
    switch (channel) {
    case LogitechChannel::shortReport: receiver.shortPath = path; break;
    case LogitechChannel::longReport: receiver.longPath = path; break;
    case LogitechChannel::headset: receiver.headsetPath = path; break;
    default: break;
    }
}

Handle open_logitech(const std::wstring& path, const LogitechReceiver& receiver, LogitechChannel expected) {
    if (path.empty()) return {};
    auto handle = open_hid(path.c_str(), GENERIC_READ | GENERIC_WRITE);
    HIDP_CAPS caps{};
    if (!handle || !hid_caps(handle.get(), logitechVendor, receiver.pid, caps) ||
        logitech_channel(caps, receiver.headset != nullptr) != expected) return {};
    return handle;
}

struct HidppReply {
    std::array<BYTE, 16> parameters{};
    std::size_t count = 0;
    BYTE error = 0;
};

std::optional<HidppReply> hidpp_reply(std::span<const BYTE> report, BYTE slot, BYTE feature, BYTE function,
                                     bool headsetRegister = false) {
    if (report.size() != 7 && report.size() != 20) return {};
    if (report[0] != (report.size() == 7 ? 0x10 : 0x11) &&
        !(headsetRegister && report[0] == 0x10)) return {};
    if (report[1] != slot) return {};
    if ((report[2] == 0x8f || report[2] == 0xff) && report[3] == feature && report[4] == function && report[5] != 0) {
        return HidppReply{{}, 0, report[5]};
    }
    if (report[2] != feature || report[3] != function) return {};
    HidppReply reply;
    reply.count = report.size() - 4;
    std::ranges::copy(report.subspan(4), reply.parameters.begin());
    return reply;
}

struct HidppChannels {
    HANDLE shortReport;
    HANDLE longReport;

    std::optional<HidppReply> request(BYTE slot, BYTE feature, BYTE function, std::array<BYTE, 3> args = {},
                                      bool useShort = false, bool headsetRegister = false) const {
        const auto output = useShort ? shortReport : longReport;
        if (!output) return {};
        std::array<BYTE, 20> packet{static_cast<BYTE>(useShort ? 0x10 : 0x11), slot, feature, function,
                                    args[0], args[1], args[2]};
        auto data = std::span(packet).first(useShort ? 7 : 20);
        if (transfer(output, data, true, 120) != data.size()) return {};
        const auto deadline = GetTickCount64() + 220;
        while (GetTickCount64() < deadline) {
            for (const bool readShort : {useShort, !useShort}) {
                const auto input = readShort ? shortReport : longReport;
                const auto now = GetTickCount64();
                if (!input || now >= deadline) continue;
                const DWORD wait = static_cast<DWORD>(std::min<ULONGLONG>(
                    shortReport && longReport ? 20 : 220, deadline - now));
                auto reply = std::span(packet).first(readShort ? 7 : 20);
                const auto count = transfer(input, reply, false, wait);
                if (count > reply.size()) continue;
                if (auto parsed = hidpp_reply(reply.first(count), slot, feature, function, headsetRegister)) return parsed;
            }
            if (GetTickCount64() < deadline) Sleep(1);
        }
        return {};
    }

    std::optional<HidppReply> feature(BYTE slot, BYTE index, BYTE function, std::array<BYTE, 3> args = {}) const {
        static std::atomic<unsigned> next{0};
        // Reserve zero for notifications; vary the software ID to reject stale replies.
        const auto softwareId = 8 + next.fetch_add(1, std::memory_order_relaxed) % 7;
        return request(slot, index, static_cast<BYTE>((function << 4) | softwareId), args);
    }

    BYTE index(BYTE slot, USHORT id) const {
        const auto result = feature(slot, 0, 0, {static_cast<BYTE>(id >> 8), static_cast<BYTE>(id), 0});
        return result && !result->error && result->parameters[0] < 0x80 ? result->parameters[0] : 0;
    }
};

struct LogitechLevel {
    std::optional<unsigned> percent;
    bool estimated = false;
};

struct VoltagePoint { unsigned mv; unsigned percent; };
constexpr VoltagePoint logitechCurve[]{
    {3500, 0}, {3579, 2}, {3646, 5}, {3671, 10}, {3717, 20}, {3751, 30}, {3778, 40}, {3811, 50},
    {3859, 60}, {3922, 70}, {3989, 80}, {4067, 90}, {4186, 100},
};
constexpr VoltagePoint g535Curve[]{{3310, 0}, {3664, 5}, {3730, 20}, {3766, 30}, {3817, 50}, {4175, 100}};

unsigned logitech_percent(unsigned mv, std::span<const VoltagePoint> curve) {
    const auto peak = std::ranges::upper_bound(curve, mv, {}, &VoltagePoint::mv);
    if (peak == curve.begin()) return 0;
    if (peak == curve.end()) return 100;
    const auto low = peak - 1;
    return low->percent + (mv - low->mv) * (peak->percent - low->percent) / (peak->mv - low->mv);
}

std::optional<LogitechLevel> hidpp_level(USHORT feature, const HidppReply& reply) {
    if (reply.error || reply.count < 3) return {};
    const auto& p = reply.parameters;
    if (feature == 0x1004 || feature == 0x1000) {
        if (p[0] > 0 && p[0] <= 100) return LogitechLevel{p[0]};
        if (feature == 0x1004 && p[0] == 0) {
            switch (p[1]) {
            case 1: return LogitechLevel{5, true};
            case 2: return LogitechLevel{20, true};
            case 4: return LogitechLevel{50, true};
            case 8: return LogitechLevel{90, true};
            }
        }
        return {};
    }
    if (feature != 0x1001 && feature != 0x1f20) return {};
    const unsigned mv = (p[0] << 8) | p[1];
    if (mv < 2500 || mv > 4500 || (feature == 0x1f20 && !(p[2] & 1))) return {};
    // Charging voltage is raised by the charger and does not give a useful state of charge.
    if (feature == 0x1001 && (p[2] & 0x80)) {
        if ((p[2] & 3) == 1) return LogitechLevel{100};
        return LogitechLevel{};
    }
    if (feature == 0x1f20 && (p[2] & 2)) return LogitechLevel{};
    return LogitechLevel{logitech_percent(mv, logitechCurve), true};
}

std::optional<LogitechLevel> hidpp_legacy_level(BYTE reg, const HidppReply& reply) {
    if (reply.error || reply.count < 3) return {};
    const auto& p = reply.parameters;
    if (reg == 0x0d) {
        if (p[0] > 0 && p[0] <= 100) return LogitechLevel{p[0]};
    } else if (reg == 0x07) {
        switch (p[0]) {
        case 1: return LogitechLevel{5, true};
        case 3: return LogitechLevel{20, true};
        case 5: return LogitechLevel{50, true};
        case 7: return LogitechLevel{90, true};
        }
    }
    return {};
}

std::optional<LogitechLevel> logitech_battery(const HidppChannels& channels, BYTE slot, USHORT pid, bool& hidpp2) {
    if (pid == 0x0ac4) {
        const auto reply = channels.request(slot, 5, 0x0d, {}, false, true);
        if (!reply || reply->error || reply->count < 3) return {};
        const auto& p = reply->parameters;
        const unsigned mv = (p[0] << 8) | p[1];
        if (mv < 2500 || mv > 4500 || (p[2] != 1 && p[2] != 3)) return {};
        return p[2] == 3 ? LogitechLevel{} : LogitechLevel{logitech_percent(mv, g535Curve), true};
    }
    const auto ping = channels.feature(slot, 0, 1, {0, 0, 0x5a});
    if (!channels.longReport || (ping && ping->error == 1)) {
        for (const BYTE reg : {0x0d, 0x07}) {
            if (auto reply = channels.request(slot, 0x81, reg, {}, true)) {
                if (auto level = hidpp_legacy_level(reg, *reply)) return level;
                if (reply->error == 8 || reply->error == 9) break;
            }
        }
        return {};
    }
    if (!ping || ping->error || ping->parameters[2] != 0x5a) return {};
    hidpp2 = true;
    for (const USHORT id : {0x1004, 0x1000, 0x1001, 0x1f20}) {
        if (const auto index = channels.index(slot, id)) {
            if (auto reply = channels.feature(slot, index, id == 0x1004 ? 1 : 0)) {
                if (auto level = hidpp_level(id, *reply)) return level;
            }
        }
    }
    return {};
}

void logitech_identity(const HidppChannels& channels, BYTE slot, Device& device) {
    const auto index = channels.index(slot, 0x0005);
    if (!index) return;
    if (const auto type = channels.feature(slot, index, 2); type && !type->error) {
        switch (type->parameters[0]) {
        case 0: case 2: device.kind = DeviceKind::keyboard; break;
        case 3: case 4: case 5: device.kind = DeviceKind::mouse; break;
        }
    }
    const auto length = channels.feature(slot, index, 0);
    if (!length || length->error) return;
    const auto limit = std::min<std::size_t>(length->parameters[0], 88);
    std::string name;
    while (name.size() < limit) {
        const auto chunk = channels.feature(slot, index, 1, {static_cast<BYTE>(name.size()), 0, 0});
        if (!chunk || chunk->error || chunk->count == 0) break;
        name.append(reinterpret_cast<const char*>(chunk->parameters.data()), std::min(chunk->count, limit - name.size()));
    }
    if (name.size() != limit || name.empty()) return;
    std::wstring wide(name.size(), L'\0');
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                           wide.data(), static_cast<int>(wide.size()));
    if (count == 0) return;
    wide.resize(static_cast<std::size_t>(count));
    auto clean = display_name(std::move(wide));
    if (!clean.empty()) device.name = std::move(clean);
}

void read_logitech(const std::map<std::wstring, LogitechReceiver>& receivers, ScanResult& result) {
    for (const auto& [key, receiver] : receivers) {
        const auto shortReport = open_logitech(receiver.shortPath, receiver, LogitechChannel::shortReport);
        auto longReport = open_logitech(receiver.longPath, receiver, LogitechChannel::longReport);
        if (!longReport) longReport = open_logitech(receiver.headsetPath, receiver, LogitechChannel::headset);
        if (!shortReport && !longReport) continue;
        const HidppChannels channels{shortReport.get(), longReport.get()};
        const BYTE first = receiver.headset ? 0xff : 1;
        const BYTE last = receiver.headset ? 0xff : 6;
        for (unsigned index = first; index <= last; ++index) {
            const auto slot = static_cast<BYTE>(index);
            bool hidpp2 = false;
            const auto level = logitech_battery(channels, slot, receiver.pid, hidpp2);
            if (!level) continue;
            auto name = receiver.headset ? std::wstring(receiver.headset) : L"Logitech #" + std::to_wstring(slot);
            Device device{std::move(name), receiver.headset ? DeviceKind::audio : DeviceKind::other,
                          true, level->percent, L"usb/logitech/" + key + L"/" + std::to_wstring(slot),
                          Transport::receiver, level->estimated};
            if (!receiver.headset && hidpp2) logitech_identity(channels, slot, device);
            result.devices.push_back(std::move(device));
        }
    }
}

} // namespace

bool has_logitech_profile(std::wstring_view path) {
    const auto pid = logitech_pid(path);
    return pid && (std::ranges::find(logitechReceivers, *pid) != std::end(logitechReceivers) ||
                   std::ranges::find(logitechHeadsets, *pid, &LogitechHeadset::pid) != std::end(logitechHeadsets));
}

void scan_logitech_receivers(std::span<const std::wstring> paths, ScanResult& result) {
    std::map<std::wstring, LogitechReceiver> receivers;
    for (const auto& path : paths) {
        if (const auto pid = logitech_pid(path)) collect_logitech(receivers, path, path, *pid);
    }
    read_logitech(receivers, result);
}
