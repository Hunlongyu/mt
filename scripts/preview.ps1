param([switch]$Settings)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $projectRoot 'out\build\win-static-release\mt.exe'
if (!(Test-Path -LiteralPath $executable)) {
    throw '请先运行 scripts/build.ps1 -QtRoot <静态 Qt SDK 路径>'
}
$dataRoot = Join-Path $projectRoot 'out\native-preview'
$arguments = @('--preview', '--data-dir', ('"{0}"' -f $dataRoot))
if ($Settings) { $arguments += '--settings' }
# This script is the explicit interactive preview entry point, not a background helper.
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Normal
