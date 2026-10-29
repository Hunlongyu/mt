param([Parameter(Mandatory=$true)][string]$QtRoot, [switch]$SkipTests, [switch]$Fresh)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'MSVC installation not found.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$qtCmake = Join-Path (Resolve-Path -LiteralPath $QtRoot).Path 'bin\qt-cmake.bat'
if (!(Test-Path -LiteralPath $qtCmake)) { throw 'Static Qt SDK is missing qt-cmake.bat.' }
$env:TOOLS_QT_ROOT = (Resolve-Path -LiteralPath $QtRoot).Path
$env:VSLANG = '1033'
$configureArguments = if ($Fresh) { '--preset win-static-release --fresh' } else { '--preset win-static-release' }
$helperDirectory = Join-Path $projectRoot 'out'
New-Item -ItemType Directory -Path $helperDirectory -Force | Out-Null
$helperPath = Join-Path $helperDirectory 'build-native.cmd'
@(
    '@echo off'
    'chcp 65001 >nul'
    "call `"$vcvars`" >nul"
    'if errorlevel 1 exit /b %errorlevel%'
    "call `"$qtCmake`" $configureArguments"
    'if errorlevel 1 exit /b %errorlevel%'
    'cmake --build --preset win-static-release'
    'exit /b %errorlevel%'
) | Set-Content -LiteralPath $helperPath -Encoding ascii
Push-Location $projectRoot
try {
    & cmd.exe /d /c $helperPath
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    if (!$SkipTests) {
        & ctest --preset win-static-release
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
    }
} finally { Pop-Location }
