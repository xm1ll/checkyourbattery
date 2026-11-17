# Check Your Battery

A lightweight native Windows 11 tray app that shows the battery level of wireless mice, headsets, keyboards, and controllers. It uses standard Windows notification icons next to the clock. Built with C++20, Win32, and C++/WinRT.

## Using the app

1. Run `CheckYourBattery-Setup.exe` and leave **Open settings and choose devices for the tray** checked on the last page. The installer adds a Start menu shortcut and supports removal from **Windows Settings → Apps → Installed apps**. To run the portable app, open `CheckYourBattery.exe` directly.
2. Left-click an icon to open settings. Check the devices you want in the tray. **Battery percentage** shows a number below each icon. Turn it off to use a small battery bar. If all devices are hidden, the battery icon still opens settings.
3. Choose **English** from the **Язык** dropdown to switch the interface from Russian. The selection applies immediately and is saved for the next launch.
4. To exit, right-click a tray icon and choose **Quit**, or use **Quit** in the settings window.

If an icon appears under the hidden-icons arrow, drag it into the main tray or enable it under Windows **Settings → Personalization → Taskbar → Other system tray icons**. [Windows taskbar settings](https://support.microsoft.com/en-us/windows/experience/personalization/customize-the-taskbar-in-windows)

## Compatibility

The app reads battery levels provided by Windows for paired Bluetooth Classic and Bluetooth Low Energy (LE) devices. It can also query the standard GATT Battery Level characteristic on connected LE devices. The tray displays one battery level per device. [Microsoft Bluetooth GATT documentation](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-client)

### 2.4 GHz USB receivers

| Device | Receiver VID:PID | Battery reading |
| --- | --- | --- |
| LAMZU Maya X (8K) | `373E:001E` | [Reported percentage](https://github.com/Sheroune/lamzu-battery-monitory) |
| Razer Barracuda X (2022, Macronix) | `1532:0552` | [Estimated from voltage](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#razer-barracuda-x-2022-24-ghz) |
| SteelSeries Arctis Nova 7 | `1038:2202`, `1038:22A1` | [Percentage or steps, depending on receiver](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#steelseries-arctis-nova-7) |
| SteelSeries Arctis Nova 7 Gen 2 | `1038:227E` | [Reported percentage](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis Nova 7X | `1038:2206`, `1038:2258`, `1038:229E`, `1038:22AD`, `1038:22A4`, `1038:22A5` | [Percentage or steps, depending on receiver](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis Nova 7P | `1038:220A`, `1038:22A7`, `1038:2298` | [Percentage or steps, depending on receiver](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis Nova 7 Diablo IV / WoW Edition | `1038:223A`, `1038:22A9`, `1038:227A` | [Percentage or steps, depending on receiver](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis Nova 5 / 5X | `1038:2232`, `1038:2253` | [Reported percentage](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis Nova 3P / 3X Wireless | `1038:2269`, `1038:226D` | [Reported percentage](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#steelseries-arctis-and-gamebuds-other-models) |
| SteelSeries Arctis 7+ and PlayStation, Xbox, Destiny editions | `1038:220E`, `1038:2212`, `1038:2216`, `1038:2236` | [Receiver battery steps](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Arctis GameBuds / GameBuds X | `1038:230A`, `1038:2317` | [Lowest level among connected earbuds](https://github.com/HeyOkay/HaloBattery/blob/main/providers/steelseries.py) |
| SteelSeries Aerox 3 Wireless / CS2 Dragon Lore | `1038:1838`, `1038:1878` | [Receiver battery level](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#steelseries-aerox-3-wireless) |
| SteelSeries Aerox 5 Wireless / Destiny 2 / Diablo IV | `1038:1852`, `1038:185C`, `1038:1860` | [Receiver battery level](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#steelseries-aerox-3-wireless) |
| SteelSeries Aerox 9 Wireless / WoW Edition | `1038:1858`, `1038:1874` | [Receiver battery level](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#steelseries-aerox-3-wireless) |
| HyperX Cloud III Wireless / Cloud II Core Wireless | `03F0:05B7`, `03F0:0995` | [Reported percentage](https://github.com/HeyOkay/HaloBattery/blob/main/providers/hyperx_cloud3.py) |
| NZXT Lift Elite Wireless | `1E71:2101` | [Reported percentage](https://github.com/HeyOkay/HaloBattery/blob/main/docs/protocols.md#nzxt-lift-elite-wireless-1e712101) |

Aerox receivers may report battery levels in 5% steps. Some headsets report battery levels in 25% steps. Firmware and hardware revisions can use different receiver IDs. The LAMZU and Razer receivers have been tested with this app. The SteelSeries, HyperX, and NZXT profiles follow published HID protocols and are awaiting hardware validation in this app. NZXT CAM can prevent the Lift Elite receiver from answering battery queries while it is running.

## Build

Run this from PowerShell on Windows 11 x64. It downloads pinned versions of LLVM-MinGW and Microsoft C++/WinRT into `.toolchain` and builds `build\CheckYourBattery.exe`.

```powershell
.\build.ps1 -Bootstrap
```

Use `.\build.ps1` for subsequent builds. The compiled executable runs on Windows 11 x64 without the build toolchain.

Build the optional installer with `.\build.ps1 -Bootstrap -Installer`. The bootstrap step fetches a pinned Inno Setup compiler into `.toolchain`. The output is `build\CheckYourBattery-Setup.exe`. Use `.\build.ps1 -Installer` for subsequent installer builds.

## Security and resource use

The app and installer run with standard user privileges. The app uses local Windows Bluetooth and HID APIs and stores preferences in `HKEY_CURRENT_USER\Software\CheckYourBattery`. The app works offline and does not install drivers, register global hotkeys, or change startup settings. The build enables ASLR, DEP, Control Flow Guard, and stack protection for the app. System DLL lookup is limited to the Windows system directory. The app and installer are unsigned.

The app scans once a minute and waits for Windows events between scans. It updates tray icons when their display changes or when Explorer restarts. The explicit `-Bootstrap` step downloads the build tools and verifies their SHA-256 checksums before extracting them.

## Demo and diagnostics

```powershell
# Show sample tray icons with temporary settings
.\build\CheckYourBattery.exe --demo

# Print detected devices and battery readings without opening the tray app
.\build\CheckYourBattery.exe --scan | Out-Host
```
