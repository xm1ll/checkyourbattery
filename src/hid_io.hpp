#pragma once

#include "battery.hpp"

#include <windows.h>
#include <hidsdi.h>

#include <memory>
#include <span>
#include <string_view>

namespace hid {
struct HandleCloser {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

Handle open_hid(const wchar_t* path, DWORD access);
bool hid_caps(HANDLE handle, USHORT vendor, USHORT product, HIDP_CAPS& caps);
DWORD transfer(HANDLE handle, std::span<BYTE> report, bool write, DWORD waitMs);
DWORD get_feature(HANDLE handle, std::span<BYTE> report, DWORD waitMs);
} // namespace hid

bool has_logitech_profile(std::wstring_view path);
void scan_logitech_receivers(std::span<const std::wstring> paths, ScanResult& result);
