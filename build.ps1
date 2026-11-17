[CmdletBinding()]
param([switch]$Bootstrap)

$ErrorActionPreference = 'Stop'
$workspace = $PSScriptRoot
$toolchain = Join-Path $workspace '.toolchain'
$buildDirectory = Join-Path $workspace 'build'
$llvmVersion = '20260908'
$winrtVersion = '2.0.250303.1'
$llvmName = "llvm-mingw-$llvmVersion-ucrt-x86_64"
$llvmBin = Join-Path $toolchain "$llvmName\bin"
$compiler = Join-Path $llvmBin 'clang++.exe'
$winrtTool = Join-Path $toolchain 'cppwinrt\bin\cppwinrt.exe'
$projection = Join-Path $toolchain 'projection'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

function Install-Archive {
    param([string]$Url, [string]$Archive, [string]$Destination, [string]$Sha256)
    if (-not (Test-Path -LiteralPath $Archive)) {
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Archive
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Checksum mismatch for $Archive. Remove that archive and retry."
    }
    New-Item -Path $Destination -ItemType Directory -Force | Out-Null
    Invoke-Checked tar.exe @('-xf', $Archive, '-C', $Destination)
}

if ($Bootstrap) {
    New-Item -Path $toolchain -ItemType Directory -Force | Out-Null
    if (-not (Test-Path -LiteralPath $compiler)) {
        Install-Archive "https://github.com/mstorsjo/llvm-mingw/releases/download/$llvmVersion/$llvmName.zip" `
            (Join-Path $toolchain 'llvm-mingw.zip') $toolchain `
            '1BCF74D06B724AEECAA6412CA85F5B26FB1DA770E7CDCEFA9263C9C5C3AD34B6'
    }
    if (-not (Test-Path -LiteralPath $winrtTool)) {
        Install-Archive "https://api.nuget.org/v3-flatcontainer/microsoft.windows.cppwinrt/$winrtVersion/microsoft.windows.cppwinrt.$winrtVersion.nupkg" `
            (Join-Path $toolchain 'cppwinrt.zip') (Join-Path $toolchain 'cppwinrt') `
            '955E3051B35DB1C00177ED14AB6B7B995C3B32A53FDB6C184EA53B1DBA66E439'
    }
}

if (-not (Test-Path -LiteralPath $compiler) -or -not (Test-Path -LiteralPath $winrtTool)) {
    throw 'Build tools are missing. Run .\build.ps1 -Bootstrap on Windows 11 x64.'
}
if (-not (Test-Path -LiteralPath (Join-Path $projection 'winrt\Windows.Devices.Bluetooth.h'))) {
    Invoke-Checked $winrtTool @('-input', (Join-Path $env:WINDIR 'System32\WinMetadata'), '-output', $projection)
}

New-Item -Path $buildDirectory -ItemType Directory -Force | Out-Null
$resource = Join-Path $buildDirectory 'widget-resource.o'
$executable = Join-Path $buildDirectory 'CheckYourBattery.exe'
Push-Location (Join-Path $workspace 'src')
try {
    Invoke-Checked (Join-Path $llvmBin 'windres.exe') @('-i', 'widget.rc', '-o', $resource, '-O', 'coff')
    Invoke-Checked $compiler @(
        '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-finput-charset=UTF-8',
        '-fstack-protector-strong', '-mguard=cf',
        '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
        '-I', $projection, '-municode', '-mwindows', '-static',
        '-Wl,--dynamicbase,--nxcompat,--high-entropy-va,--dependent-load-flag=0x800',
        'tray.cpp', 'tray_icon.cpp', 'battery.cpp', 'usb_battery.cpp', $resource, '-o', $executable,
        '-luser32', '-lgdi32', '-lshell32', '-lcomctl32', '-ladvapi32', '-lbcrypt', '-lcfgmgr32',
        '-lsetupapi', '-lhid', '-lruntimeobject', '-lole32', '-loleaut32'
    )
} finally {
    Pop-Location
}
Write-Host "Built $executable"
