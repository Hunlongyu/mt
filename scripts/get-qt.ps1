param(
    [string]$InstallRoot = 'C:\mt-deps\sdk',
    [string]$ArchiveRoot = 'C:\mt-deps\archives'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$manifest = Get-Content -Raw "$PSScriptRoot/dependencies.json" | ConvertFrom-Json
New-Item -ItemType Directory -Path $InstallRoot,$ArchiveRoot -Force | Out-Null
function Get-VerifiedArchive([string]$url, [string]$file, [string]$sha256) {
    $destination = Join-Path $ArchiveRoot $file
    if (!(Test-Path -LiteralPath $destination)) {
        Write-Output "下载 $file"
        Invoke-WebRequest -Uri $url -OutFile ($destination + '.part') -MaximumRetryCount 3 -TimeoutSec 600
        if ((Get-FileHash -LiteralPath ($destination + '.part') -Algorithm SHA256).Hash -ine $sha256) { throw "校验失败：$file" }
        Move-Item -LiteralPath ($destination + '.part') -Destination $destination
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine $sha256) { throw "缓存校验失败：$file" }
}
Get-VerifiedArchive $manifest.sdk.url $manifest.sdk.file $manifest.sdk.sha256
$sdk = Join-Path $InstallRoot $manifest.sdk.directory
if (!(Test-Path -LiteralPath (Join-Path $sdk 'bin/qt-cmake.bat'))) {
    $sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
    if ($sevenZip) {
        & $sevenZip.Source x (Join-Path $ArchiveRoot $manifest.sdk.file) "-o$InstallRoot" -y -bso0 -bsp0
    } else {
        & "$env:SystemRoot/System32/tar.exe" -xf (Join-Path $ArchiveRoot $manifest.sdk.file) -C $InstallRoot
    }
    if ($LASTEXITCODE -ne 0) { throw 'Qt SDK 解压失败。' }
}
foreach ($file in @('bin/qt-cmake.bat','lib/Qt6Core.lib','lib/Qt6Widgets.lib','lib/Qt6Sql.lib','lib/Qt6Svg.lib','lib/Qt6Network.lib','lib/Qt6Test.lib','plugins/platforms/qwindows.lib','plugins/sqldrivers/qsqlite.lib','plugins/tls/qschannelbackend.lib')) {
    if (!(Test-Path -LiteralPath (Join-Path $sdk $file))) { throw "Qt SDK 缺少 $file" }
}
# Release packages also carry the corresponding source, not only an upstream link.
foreach ($module in $manifest.archives) { Get-VerifiedArchive ($manifest.baseUrl + $module.file) $module.file $module.sha256 }
Write-Output "静态 Qt 已就绪：$sdk"
