[Setup]
AppId=CheckYourBattery
AppName=Check Your Battery
AppVersion=1.1.0
AppPublisher=xm1ll
AppPublisherURL=https://github.com/xm1ll/checkyourbattery
DefaultDirName={localappdata}\Programs\CheckYourBattery
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
MinVersion=10.0.22000
AppMutex=Local\CheckYourBattery.Tray,Local\CheckYourBattery.Tray.Demo
UninstallDisplayIcon={app}\CheckYourBattery.exe
SetupIconFile=src\battery.ico
OutputDir=build
OutputBaseFilename=CheckYourBattery-Setup
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "build\CheckYourBattery.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Check Your Battery"; Filename: "{app}\CheckYourBattery.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\CheckYourBattery.exe"; Parameters: "--configure"; Description: "{cm:ConfigureDevices}"; Flags: nowait postinstall skipifsilent

[CustomMessages]
english.ConfigureDevices=Open settings and choose devices for the tray
russian.ConfigureDevices=Открыть настройки и выбрать устройства для трея
