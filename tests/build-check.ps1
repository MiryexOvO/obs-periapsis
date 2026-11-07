param(
    [string] $Core = (Join-Path $PSScriptRoot '../../periapsis'),
    [string] $Vs = 'D:/VS',
    [string] $Sdk = 'D:/Windows Kits/10',
    [string] $ObsBin = 'D:/OBS/obs-studio/bin/64bit'
)
$ErrorActionPreference = 'Stop'
$Core = (Resolve-Path $Core).Path
$msvc = (Get-ChildItem "$Vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkVersion = (Get-ChildItem "$Sdk/Lib" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$env:PATH = "$msvc/bin/Hostx64/x64;$ObsBin;$env:PATH"
$env:INCLUDE = "$msvc/include;$Sdk/Include/$sdkVersion/ucrt;$Sdk/Include/$sdkVersion/shared;$Sdk/Include/$sdkVersion/um"
$env:LIB = "$msvc/lib/x64;$Sdk/Lib/$sdkVersion/ucrt/x64;$Sdk/Lib/$sdkVersion/um/x64"
$headers = Join-Path $Core 'target/obs-sdk'
& (Join-Path $PSScriptRoot '../native/build.ps1') -ObsInclude $headers -ObsLibrary "$headers/obs.lib" -Core $Core
Push-Location $headers
try {
    foreach ($test in @('recovery-gate', 'uplink-budget', 'encoder-response', 'key-trace', 'ring-pressure')) {
        & cl.exe /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /I $headers "$PSScriptRoot/$test.cpp" /link "$headers/obs.lib" "/OUT:$headers/$test.exe"
        if ($LASTEXITCODE) { throw "$test compilation failed" }
        & "$headers/$test.exe"
        if ($LASTEXITCODE) { throw "${test} failed: $LASTEXITCODE" }
    }
    & cl.exe /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /I $headers "$PSScriptRoot/obs-recovery-source.cpp" /link "$headers/obs.lib" "/OUT:$headers/obs-recovery-source.exe"
    if ($LASTEXITCODE) { throw 'OBS source fixture compilation failed' }
} finally { Pop-Location }
