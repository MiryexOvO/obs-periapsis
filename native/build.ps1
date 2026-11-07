param(
    [Parameter(Mandatory)] [string] $ObsInclude,
    [Parameter(Mandatory)] [string] $ObsLibrary,
    [string] $Core = (Join-Path $PSScriptRoot '../../periapsis'),
    [string] $Qt = ''
)
$ErrorActionPreference = 'Stop'
$Core = (Resolve-Path $Core).Path
if (!$Qt) { $Qt = Join-Path $Core 'target/obs-sdk/qt6' }
if (!(Test-Path "$Qt/include/QtWidgets/QComboBox")) { throw 'Qt 6 SDK required: pass -Qt with matching OBS Qt SDK path.' }
& cargo build --release --locked --manifest-path (Join-Path $Core 'Cargo.toml') -p peri-obs
if ($LASTEXITCODE) { throw 'QUIC library build failed / QUIC 库构建失败' }
$build = Join-Path $Core 'target/obs-package'
$payload = Join-Path $build 'peri-output/bin/64bit'
New-Item -ItemType Directory -Force $payload | Out-Null
Push-Location $build
try {
    # Link the C ABI import library; ship the matching runtime beside the module. / 通过导入库调用同目录的 Rust 动态库。
    & cl.exe /nologo /std:c++17 /Zc:__cplusplus /permissive- /EHsc /utf-8 /MD /O2 /LD /I $ObsInclude /I "$Qt/include" /I "$Qt/include/QtCore" /I "$Qt/include/QtGui" /I "$Qt/include/QtWidgets" (Join-Path $PSScriptRoot 'peri-output.cpp') (Join-Path $PSScriptRoot 'settings-ui.cpp') /link $ObsLibrary (Join-Path $Core 'target/release/peri_obs.dll.lib') "$Qt/lib/Qt6Core.lib" "$Qt/lib/Qt6Gui.lib" "$Qt/lib/Qt6Widgets.lib" ws2_32.lib userenv.lib bcrypt.lib ntdll.lib advapi32.lib crypt32.lib secur32.lib kernel32.lib "/OUT:$payload/peri-output.dll" /IMPLIB:peri-output.lib
    if ($LASTEXITCODE) { throw 'OBS module build failed / OBS 模块构建失败' }
    Copy-Item -Force -LiteralPath (Join-Path $Core 'target/release/peri_obs.dll') -Destination $payload
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-PERI.cmd') -Destination .
    $zip = Join-Path $PSScriptRoot '../PERI-OBS-Windows-x64.zip'
    Compress-Archive -Force -Path peri-output,Install-PERI.cmd -DestinationPath $zip
    Write-Output $zip
} finally { Pop-Location }
