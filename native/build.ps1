param(
    [Parameter(Mandatory)] [string] $ObsInclude,
    [Parameter(Mandatory)] [string] $ObsLibrary
)
# Use the x64 MSVC developer shell. / 请使用 x64 MSVC 开发者终端。
$buildDir = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString()))
Push-Location $buildDir.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /LD /I $ObsInclude (Join-Path $PSScriptRoot 'peri-output.cpp') /link $ObsLibrary /OUT:peri-output.dll
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed / 原生模块构建失败' }
    Copy-Item -LiteralPath 'peri-output.dll' -Destination $PSScriptRoot -ErrorAction Stop
} finally {
    Pop-Location
}
