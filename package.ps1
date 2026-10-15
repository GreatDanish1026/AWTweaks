# Builds, tests and packages a release: dist\AWTweaks-<version>.zip (+ .sha256).
# Usage: .\package.ps1 [-SkipTests]
param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$version = (Select-String -Path "$root\src\version.h" -Pattern '#define AWT_VERSION_STRING "([^"]+)"').Matches[0].Groups[1].Value
if (-not $version) { throw "version not found in src\version.h" }
Write-Host "AWTweaks $version"

& "$root\build.ps1"

if (-not $SkipTests) {
    # Offline harness: fake engine structures with the game's exact code signatures.
    $cxx = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\*\bin\clang++.exe" |
        Select-Object -First 1 -ExpandProperty FullName
    $t = Join-Path $env:TEMP "awtweaks_release_test"
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $t | Out-Null
    & $cxx --target=x86_64-w64-mingw32 -std=c++17 -O1 -municode -static -o "$t\harness.exe" "$root\test\harness.cpp"
    if ($LASTEXITCODE) { throw "harness build failed" }
    Copy-Item "$root\bin\xinput1_4.dll" $t
    $out = & "$t\harness.exe" $t
    if ($LASTEXITCODE) { $out | Select-String 'FAIL' | ForEach-Object { Write-Host $_ }; throw "harness tests failed" }
    Write-Host "Harness: all checks passed"
}

$name = "AWTweaks-$version"
$stage = "$root\dist\$name"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$root\bin\xinput1_4.dll", "$root\bin\AWTweaks.addon64", "$root\awtweaks.ini", `
          "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage

# The packaged binaries must carry this version.
foreach ($f in 'xinput1_4.dll', 'AWTweaks.addon64') {
    $v = (Get-Item "$stage\$f").VersionInfo.FileVersion
    if ($v -ne $version) { throw "$f has version '$v', expected $version" }
}

$zip = "$root\dist\$name.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content "$zip.sha256" -Encoding ascii
Write-Host "Packaged $zip"
Write-Host "SHA-256 $hash"
