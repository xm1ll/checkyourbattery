#include "battery.hpp"
#include "resource.h"
#include "tray_icon.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <thread>

namespace {
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT_PTR scanTimer = 1;
constexpr UINT scanInterval = 60000;
constexpr wchar_t applicationName[] = L"Check Your Battery";
constexpr wchar_t preferencesKey[] = L"Software\\CheckYourBattery";
constexpr wchar_t percentPreference[] = L"ShowBatteryPercent";

struct ScanWork {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ScanResult result;
    ~ScanWork() { if (done) CloseHandle(done); }
};

struct IconSpec {
    DeviceKind kind = DeviceKind::other;
    std::optional<unsigned> percent;
    std::wstring tooltip;
    bool showPercent = false;
    bool operator==(const IconSpec&) const = default;
};

std::optional<GUID> icon_guid(const std::wstring& key) {
    auto input = L"CheckYourBattery/tray/v1/" + key;
    std::array<UCHAR, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
                   static_cast<ULONG>(input.size() * sizeof(wchar_t)), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        return {};
    GUID id{};
    std::memcpy(&id, digest.data(), sizeof(id));
    id.Data3 = static_cast<unsigned short>((id.Data3 & 0x0fff) | 0x8000);
    id.Data4[0] = (id.Data4[0] & 0x3f) | 0x80;
    return id;
}

std::wstring device_preference(const std::wstring& key) {
    const auto guid = icon_guid(key);
    wchar_t identifier[39]{};
    if (!guid || !StringFromGUID2(*guid, identifier, ARRAYSIZE(identifier))) return {};
    return std::wstring(L"Hidden.") + identifier;
}

bool read_preference(const wchar_t* name, bool fallback = false) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, preferencesKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value == 1;
}

bool write_preference(const wchar_t* name, bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, preferencesKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = enabled ? 1 : 0;
    const auto status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

struct TrayIcon {
    GUID guid;
    UINT id;
    bool installed = false;
    IconSpec current;

    NOTIFYICONDATAW payload(HWND window) const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = id;
        data.uFlags = NIF_GUID;
        data.guidItem = guid;
        return data;
    }

    void update(HWND window, const IconSpec& desired) {
        if (installed && current == desired) return;
        auto data = payload(window);
        data.uFlags |= NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = trayMessage;
        lstrcpynW(data.szTip, desired.tooltip.c_str(), ARRAYSIZE(data.szTip));

        HICON image = nullptr;
        if (!installed || current.kind != desired.kind || current.percent != desired.percent ||
            current.showPercent != desired.showPercent) {
            image = create_tray_icon(desired.kind, desired.percent, desired.showPercent);
            if (!image) return;
            data.uFlags |= NIF_ICON;
            data.hIcon = image;
        }
        const bool applied = Shell_NotifyIconW(installed ? NIM_MODIFY : NIM_ADD, &data) != FALSE;
        if (image) DestroyIcon(image);
        if (!applied) {
            installed = false;
            return;
        }
        if (!installed) {
            auto version = payload(window);
            version.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &version);
        }
        installed = true;
        current = desired;
    }

    void remove(HWND window) const {
        auto data = payload(window);
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
};

class App {
public:
    explicit App(bool demo) : demo(demo) {}

    HWND window = nullptr;
    const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    void initialize() {
        showPercent = !demo && read_preference(percentPreference, true);
        if (demo) {
            last = demo_devices();
            render();
        } else {
            start_scan();
            render();
            SetTimer(window, scanTimer, scanInterval, nullptr);
        }
    }

    void start_scan() {
        if (demo || work) return;
        try {
            auto pending = std::make_shared<ScanWork>();
            if (!pending->done) {
                failed = true;
                render();
                return;
            }
            std::thread([pending] {
                try { pending->result = scan_devices(); }
                catch (...) { pending->result.failed = true; }
                SetEvent(pending->done);
            }).detach();
            work = std::move(pending);
        } catch (const std::exception&) {
            failed = true;
            render();
        }
    }

    HANDLE scan_event() const { return work ? work->done : nullptr; }

    void complete_scan() {
        failed = work->result.failed;
        if (!failed) {
            auto& incoming = work->result;
            for (auto& device : last.devices) {
                if (device.transport == Transport::receiver ? incoming.receiverFailed : incoming.bluetoothFailed) {
                    device.percent.reset();
                    if (std::ranges::none_of(incoming.devices, [&](const Device& item) { return item.id == device.id; }))
                        incoming.devices.push_back(std::move(device));
                }
            }
            last = std::move(incoming);
        }
        work.reset();
        render();
        if (settings) refresh_device_list();
    }

    void recreate_icons() {
        for (auto& [key, icon] : icons) icon.installed = false;
        render();
    }

    void remove_icons() const {
        for (const auto& [key, icon] : icons) icon.remove(window);
    }

    HWND settings_window() const { return settings; }

    void open_settings() {
        if (!settings) {
            const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
            if (!InitCommonControlsEx(&controls)) return;
            if (!CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), window,
                                    settings_proc, reinterpret_cast<LPARAM>(this))) return;
            POINT cursor{};
            RECT bounds{};
            MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};
            if (GetCursorPos(&cursor) && GetWindowRect(settings, &bounds) &&
                GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const auto& area = monitor.rcWork;
                const auto width = bounds.right - bounds.left;
                const auto height = bounds.bottom - bounds.top;
                const auto x = std::clamp(cursor.x - width, area.left, std::max(area.left, area.right - width));
                const auto y = std::max(area.top, area.bottom - height - 10);
                SetWindowPos(settings, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        if (IsIconic(settings)) ShowWindow(settings, SW_RESTORE);
        SetWindowPos(settings, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
        SetForegroundWindow(settings);
    }

    void open_tray_menu() {
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, 1, L"Настройки");
        AppendMenuW(menu, MF_STRING, 2, L"Выйти");
        POINT anchor{};
        GetCursorPos(&anchor);
        TPMPARAMS placement{};
        placement.cbSize = sizeof(placement);
        MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};
        const bool hasMonitor = GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
        if (hasMonitor) {
            placement.rcExclude = {monitor.rcMonitor.left, monitor.rcWork.bottom,
                                   monitor.rcMonitor.right, monitor.rcMonitor.bottom};
            anchor.y = std::min(anchor.y - 2, monitor.rcWork.bottom - 2);
        }
        SetForegroundWindow(window);
        const UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_RIGHTALIGN |
                                                      TPM_BOTTOMALIGN | TPM_VERTICAL, anchor.x, anchor.y, window,
                                               hasMonitor ? &placement : nullptr);
        DestroyMenu(menu);
        if (selected == 2) DestroyWindow(window);
        else {
            if (selected == 1) open_settings();
            PostMessageW(window, WM_NULL, 0, 0);
        }
    }

private:
    bool device_hidden(const std::wstring& key) {
        const auto [it, inserted] = hidden.try_emplace(key, false);
        if (inserted && !demo) {
            const auto setting = device_preference(key);
            it->second = !setting.empty() && read_preference(setting.c_str());
        }
        return it->second;
    }

    void refresh_device_list() {
        std::map<std::wstring, std::wstring> names;
        for (const auto& device : last.devices) {
            auto label = device.name;
            if (device.transport == Transport::receiver) label += L" · 2,4 ГГц";
            names.try_emplace(device.id, std::move(label));
        }
        std::vector<std::pair<std::wstring, std::wstring>> rows(names.begin(), names.end());
        std::ranges::sort(rows, {}, &std::pair<std::wstring, std::wstring>::second);
        const auto list = GetDlgItem(settings, IDC_DEVICES);
        ShowWindow(GetDlgItem(settings, IDC_EMPTY), rows.empty() ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(settings, IDC_EMPTY, failed ? L"Не удалось прочитать список устройств."
                                                   : work ? L"Поиск устройств…" : L"Нет Bluetooth-устройств или поддерживаемых приёмников.");
        if (rows == settingsRows) {
            ShowWindow(list, rows.empty() ? SW_HIDE : SW_SHOW);
            return;
        }

        updatingSettings = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        settingsRows = std::move(rows);
        for (int i = 0; i < static_cast<int>(settingsRows.size()); ++i) {
            auto& [key, name] = settingsRows[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = i;
            item.pszText = name.data();
            item.lParam = i;
            const int index = ListView_InsertItem(list, &item);
            if (index >= 0) ListView_SetCheckState(list, index, !device_hidden(key));
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(list, settingsRows.empty() ? SW_HIDE : SW_SHOW);
        InvalidateRect(list, nullptr, TRUE);
        updatingSettings = false;
    }

    void initialize_settings(HWND dialog) {
        settings = dialog;
        SetWindowTextW(dialog, demo ? L"Настройки заряда — демо" : L"Настройки заряда");
        SetDlgItemTextW(dialog, IDC_DEVICE_LABEL, L"Устройства в области уведомлений");
        SetDlgItemTextW(dialog, IDC_PERCENT, L"Зарядка в процентах");
        SetDlgItemTextW(dialog, IDC_DONE, L"Готово");
        SetDlgItemTextW(dialog, IDC_QUIT, L"Выйти");
        CheckDlgButton(dialog, IDC_PERCENT, showPercent ? BST_CHECKED : BST_UNCHECKED);
        const auto list = GetDlgItem(dialog, IDC_DEVICES);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
        RECT bounds{};
        GetClientRect(list, &bounds);
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH;
        column.cx = bounds.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(dialog));
        ListView_InsertColumn(list, 0, &column);
        refresh_device_list();
    }

    void change_percent() {
        const bool enabled = IsDlgButtonChecked(settings, IDC_PERCENT) == BST_CHECKED;
        if (!demo && !write_preference(percentPreference, enabled)) {
            CheckDlgButton(settings, IDC_PERCENT, showPercent ? BST_CHECKED : BST_UNCHECKED);
            settings_error();
            return;
        }
        showPercent = enabled;
        render();
    }

    void change_device(const NMLISTVIEW& notification) {
        if (updatingSettings || notification.iItem < 0 || !(notification.uChanged & LVIF_STATE) ||
            !((notification.uOldState ^ notification.uNewState) & LVIS_STATEIMAGEMASK)) return;
        const auto row = notification.lParam;
        if (row < 0 || static_cast<size_t>(row) >= settingsRows.size()) return;
        const auto& key = settingsRows[static_cast<size_t>(row)].first;
        const bool hideDevice = !ListView_GetCheckState(notification.hdr.hwndFrom, notification.iItem);
        if (!demo) {
            const auto preference = device_preference(key);
            if (preference.empty() || !write_preference(preference.c_str(), hideDevice)) {
                updatingSettings = true;
                ListView_SetCheckState(notification.hdr.hwndFrom, notification.iItem, !device_hidden(key));
                updatingSettings = false;
                settings_error();
                return;
            }
        }
        hidden[key] = hideDevice;
        render();
    }

    void settings_error() const {
        MessageBoxW(settings, L"Не удалось сохранить настройки.", applicationName, MB_OK | MB_ICONWARNING);
    }

    static INT_PTR CALLBACK settings_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INITDIALOG) {
            auto* app = reinterpret_cast<App*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(app));
            app->initialize_settings(dialog);
            return TRUE;
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (!app) return FALSE;
        switch (message) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_PERCENT && HIWORD(wParam) == BN_CLICKED) app->change_percent();
            else if (LOWORD(wParam) == IDC_DONE && HIWORD(wParam) == BN_CLICKED) DestroyWindow(dialog);
            else if (LOWORD(wParam) == IDC_QUIT && HIWORD(wParam) == BN_CLICKED)
                PostMessageW(app->window, WM_CLOSE, 0, 0);
            else return FALSE;
            return TRUE;
        case WM_NOTIFY: {
            const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
            if (notification && notification->idFrom == IDC_DEVICES && notification->code == LVN_ITEMCHANGED)
                app->change_device(*reinterpret_cast<const NMLISTVIEW*>(lParam));
            return FALSE;
        }
        case WM_CLOSE:
            DestroyWindow(dialog);
            return TRUE;
        case WM_DESTROY:
            app->settings = nullptr;
            app->settingsRows.clear();
            return TRUE;
        }
        return FALSE;
    }

    void render() {
        std::map<std::wstring, IconSpec> desired;
        for (const auto& device : last.devices) {
            if (device_hidden(device.id)) continue;
            const auto level = !failed && device.connected ? device.percent : std::nullopt;
            std::wstring tooltip = demo ? L"Демо · " : L"";
            tooltip += device.name;
            tooltip += device.transport == Transport::receiver ? L" · 2,4 ГГц · " : L" · ";
            if (failed) tooltip += L"Чтение недоступно";
            else if (!device.connected) tooltip += device.transport == Transport::receiver ? L"Нет ответа от устройства" : L"Не подключено";
            else if (level) {
                if (device.estimated) tooltip += L"≈";
                tooltip += std::to_wstring(*level) + L"%";
            }
            else tooltip += L"Нет данных о заряде";
            desired.try_emplace(device.id, IconSpec{device.kind, level, std::move(tooltip), showPercent && level.has_value()});
        }
        if (desired.empty()) {
            const auto tip = !last.devices.empty() ? L"Заряд устройств · Все устройства скрыты"
                           : failed ? L"Заряд устройств · Ошибка чтения"
                           : work ? L"Заряд устройств · Поиск устройств"
                                  : L"Заряд устройств · Нет поддерживаемых устройств";
            desired.emplace(L"status", IconSpec{DeviceKind::other, {}, tip});
        }

        for (auto it = icons.begin(); it != icons.end();) {
            if (desired.contains(it->first)) { ++it; continue; }
            it->second.remove(window);
            it = icons.erase(it);
        }
        for (const auto& [key, spec] : desired) {
            auto it = icons.find(key);
            if (it == icons.end()) {
                const auto guid = icon_guid(key);
                if (!guid) continue;
                it = icons.emplace(key, TrayIcon{*guid, ++nextIconId, false, {}}).first;
            }
            it->second.update(window, spec);
        }
    }

    const bool demo;
    bool showPercent = false;
    bool updatingSettings = false;
    bool failed = false;
    UINT nextIconId = 0;
    ScanResult last;
    std::shared_ptr<ScanWork> work;
    std::map<std::wstring, TrayIcon> icons;
    std::map<std::wstring, bool> hidden;
    HWND settings = nullptr;
    std::vector<std::pair<std::wstring, std::wstring>> settingsRows;
};

LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    if (app->taskbarCreated && message == app->taskbarCreated) {
        app->recreate_icons();
        return 0;
    }
    switch (message) {
    case WM_TIMER:
        if (wParam == scanTimer) app->start_scan();
        return 0;
    case trayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || lParam == WM_RBUTTONUP) app->open_tray_menu();
        else if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT || lParam == WM_LBUTTONUP)
            app->open_settings();
        return 0;
    case WM_ENDSESSION:
        if (wParam) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, scanTimer);
        app->remove_icons();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int run(App& app) {
    for (;;) {
        const HANDLE pending = app.scan_event();
        const DWORD count = pending ? 1u : 0u;
        const DWORD signal = MsgWaitForMultipleObjectsEx(count, pending ? &pending : nullptr,
                                                          INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (signal == WAIT_FAILED) return 1;
        if (pending && signal == WAIT_OBJECT_0) app.complete_scan();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
            if (const auto dialog = app.settings_window(); dialog && IsDialogMessageW(dialog, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

int print_scan() {
    const auto result = scan_devices();
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring report = result.failed ? L"Device scan failed\r\n" : L"Device scan completed\r\n";
    if (result.receiverFailed) report += L"USB receiver scan failed\r\n";
    if (result.bluetoothFailed) report += L"Bluetooth scan failed\r\n";
    if (!result.failed && result.devices.empty()) report += L"No supported devices\r\n";
    for (const auto& device : result.devices) {
        if (device.estimated && device.percent) report += L"~";
        report += device.percent ? std::to_wstring(*device.percent) + L"%" : L"--";
        report += device.connected ? L"\tconnected\t" : device.transport == Transport::receiver ? L"\tno response\t" : L"\tdisconnected\t";
        report += device.transport == Transport::receiver ? L"2.4 GHz\t" : L"Bluetooth\t";
        report += device.name + L"\r\n";
    }
    DWORD written = 0;
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(output, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
    } else if (output && output != INVALID_HANDLE_VALUE) {
        const int size = static_cast<int>(report.size());
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, report.data(), size, nullptr, 0, nullptr, nullptr);
        std::string utf8(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, report.data(), size, utf8.data(), bytes, nullptr, nullptr);
        WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    return result.failed ? 1 : 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return 1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring_view option = argv && argc > 1 ? argv[1] : L"";
    const bool demo = option == L"--demo";
    const bool diagnostic = option == L"--scan";
    LocalFree(argv);
    if (diagnostic) return print_scan();

    const wchar_t* windowClass = demo ? L"CheckYourBattery.Tray.Demo" : L"CheckYourBattery.Tray";
    const auto mutexName = std::wstring(L"Local\\") + windowClass;
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) return 1;
    const bool duplicate = GetLastError() == ERROR_ALREADY_EXISTS;
    if (duplicate) { CloseHandle(mutex); return 0; }

    App app(demo);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = tray_proc;
    cls.lpszClassName = windowClass;
    const HWND window = RegisterClassW(&cls) ? CreateWindowExW(WS_EX_TOOLWINDOW, windowClass, applicationName,
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, &app) : nullptr;
    int result = 1;
    if (window) {
        app.initialize();
        result = run(app);
        if (IsWindow(window)) DestroyWindow(window);
    }
    CloseHandle(mutex);
    return result;
}
