# Builds bin\xinput1_4.dll with LLVM-MinGW (winget: MartinStorsjo.LLVM-MinGW.UCRT).
# Usage: .\build.ps1 [-Install "E:\Non-Steam Games\AlanWakeRemastered"]
param([string]$Install)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$cxx = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $cxx) {
    $cxx = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\*\bin\clang++.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cxx) { throw "clang++ not found. Install with: winget install MartinStorsjo.LLVM-MinGW.UCRT" }

New-Item -ItemType Directory -Force "$root\bin" | Out-Null

# The shipped ini must keep every section the DLL reads; a truncated copy once
# dropped AtlasScale and froze the game. Fail the build instead of shipping that.
$ini = Get-Content "$root\awtweaks.ini" -Raw
$required = @('(?m)^\[Settings\]', '(?m)^Preset=', '(?m)^\[Shadows\]', '(?m)^AtlasScale=', '(?m)^LightShadowScale=',
              '(?m)^\[FreeCamera\]', '(?m)^\[PhotoMode\]', '(?m)^\[Debug\]', '(?m)^\[Tweaks\]', '(?m)^\[Preset:')
$missing = $required | Where-Object { $ini -notmatch $_ }
if ($missing) { throw "awtweaks.ini is missing: $($missing -join ', ')" }
& $cxx --target=x86_64-w64-mingw32 -std=c++17 -O2 -shared -static -s `
    -Wall -Wno-unused-function `
    -o "$root\bin\xinput1_4.dll" "$root\src\awtweaks.cpp" "$root\src\xinput1_4.def"
if ($LASTEXITCODE) { throw "build failed" }
Copy-Item "$root\awtweaks.ini" "$root\bin\awtweaks.ini" -Force
Write-Host "Built $root\bin\xinput1_4.dll"

# ReShade add-on (in-game slider window). Headers in third_party/ match ReShade 6.8.0.
& $cxx --target=x86_64-w64-mingw32 -std=c++17 -O2 -shared -static -s `
    -Wall -Wno-unused-function -Wno-missing-braces -Wno-unknown-attributes `
    -I "$root\third_party\imgui" -I "$root\third_party\reshade\include" `
    -o "$root\bin\AWTweaks.addon64" "$root\src\addon\awtweaks_overlay.cpp"
if ($LASTEXITCODE) { throw "add-on build failed" }
Write-Host "Built $root\bin\AWTweaks.addon64"

if ($Install) {
    Copy-Item "$root\bin\xinput1_4.dll" $Install -Force
    Copy-Item "$root\bin\AWTweaks.addon64" $Install -Force
    if (-not (Test-Path "$Install\awtweaks.ini")) { Copy-Item "$root\awtweaks.ini" $Install }
    Write-Host "Installed to $Install"
}
