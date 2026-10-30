param(
    [string]$BuildRoot = 'out/build/win-static-release',
    [string]$Destination = 'dist',
    [Parameter(Mandatory=$true)][string]$ArchiveRoot,
    [string]$Tag = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/msvc.ps1"
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
if (!(Test-Path -LiteralPath $tar)) { throw '缺少 Windows 系统 tar.exe。' }
$project = Split-Path $PSScriptRoot -Parent
Push-Location $project
try {
    Write-Output "发布打包使用系统 tar：$tar"
    Write-Output '检查版本与源码状态'
    $cmakeText = Get-Content -Raw 'CMakeLists.txt'
    if ($cmakeText -notmatch 'project\(mt VERSION (\d+\.\d+\.\d+)') { throw '无法读取项目版本。' }
    $version = $matches[1]
    if ($Tag -and $Tag -notmatch ('^v' + [regex]::Escape($version) + '(-[0-9A-Za-z.-]+)?$')) { throw '标签与 CMake 版本不一致。' }
    $exe = Join-Path $BuildRoot 'mt.exe'
    if (!(Test-Path -LiteralPath $exe)) { throw '缺少 mt.exe。' }
    if ((Get-Item -LiteralPath $exe).VersionInfo.ProductVersion -ne $version) { throw 'EXE 与 CMake 版本不一致。' }
    $dependencyManifest = Get-Content -Raw 'scripts/dependencies.json' | ConvertFrom-Json
    $cache = Get-Content -Raw -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')
    if ($cache -notmatch '(?m)^Qt6Core_DIR:PATH=([^\r\n]+)') { throw '无法确定构建所用 Qt。' }
    $qtConfig = Get-Content -Raw -LiteralPath (Join-Path $matches[1] 'Qt6CoreConfigVersionImpl.cmake')
    if ($qtConfig -notmatch 'set\(PACKAGE_VERSION "([^"\r\n]+)"\)' -or $matches[1] -ne $dependencyManifest.qtVersion) {
        throw '构建所用 Qt 版本与对应源码材料不一致。'
    }
    if (& git status --porcelain --untracked-files=no) { throw '请先提交受控文件变更，确保源码包与构建代码一致。' }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $stage = Join-Path $project ('out/package-' + [guid]::NewGuid().ToString('N'))
    $runtime = Join-Path $stage 'runtime'
    $source = Join-Path $stage 'source'
    New-Item -ItemType Directory -Path $runtime,$source -Force | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $runtime 'mt.exe')
    Copy-Item -LiteralPath 'LICENSE','THIRD_PARTY_NOTICES.md' -Destination $runtime
    Copy-Item -LiteralPath 'resources/licenses' -Destination (Join-Path $runtime 'licenses') -Recurse
    Copy-Item -LiteralPath 'resources/icons/lucide/LICENSE.txt' -Destination (Join-Path $runtime 'licenses/Lucide.txt')
    $qtNotices = Join-Path $runtime 'licenses/qt'
    New-Item -ItemType Directory -Path $qtNotices -Force | Out-Null
    foreach ($module in $dependencyManifest.archives) {
        Write-Output "校验并提取 $($module.name) 许可"
        $archive = Join-Path $ArchiveRoot $module.file
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $module.sha256) { throw "Qt 源码校验失败：$archive" }
        Write-Output "$($module.name) SHA-256 已通过，开始流式提取"
        & python "$PSScriptRoot/extract-qt.py" $archive $qtNotices --licenses
        Assert-NativeSuccess '提取 Qt 与内置组件许可'
    }
    Copy-Item -LiteralPath $exe -Destination (Join-Path $Destination 'mt.exe')
    Write-Output '生成便携包'
    Compress-Archive -Path "$runtime/*" -DestinationPath (Join-Path $Destination "mt-$version-windows-x64.zip") -Force
    # Complete application source + exact Qt and WIL source, sufficient to rebuild/relink.
    $sourceTar = Join-Path $stage 'project.tar'
    Write-Output '归档应用与依赖源码'
    & git archive --format=tar --output=$sourceTar HEAD
    Assert-NativeSuccess '打包项目源码'
    & $tar -xf $sourceTar -C $source
    Assert-NativeSuccess '展开项目源码'
    $archives = Join-Path $source 'dependencies'
    New-Item -ItemType Directory -Path $archives -Force | Out-Null
    foreach ($module in $dependencyManifest.archives) {
        $archive = Join-Path $ArchiveRoot $module.file
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $module.sha256) { throw "Qt 源码校验失败：$archive" }
        Copy-Item -LiteralPath $archive -Destination $archives
    }
    $wil = Join-Path $BuildRoot '_deps/wil-src'
    if (!(Test-Path -LiteralPath "$wil/include/wil/resource.h")) { throw '缺少构建所用的 WIL 源码。' }
    $wilDestination = Join-Path $source 'third_party/wil'
    New-Item -ItemType Directory -Path $wilDestination -Force | Out-Null
    Get-ChildItem -LiteralPath $wil -Force | Where-Object Name -ne '.git' | Copy-Item -Destination $wilDestination -Recurse
    Copy-Item -LiteralPath 'docs/releasing.md' -Destination (Join-Path $source 'REBUILD.md')
    Write-Output '生成完整源码包'
    Compress-Archive -Path "$source/*" -DestinationPath (Join-Path $Destination "mt-$version-source-with-dependencies.zip") -Force
    $sums = Get-ChildItem -LiteralPath $Destination -File | Where-Object { $_.Extension -in @('.exe','.zip') } | Sort-Object Name | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
    }
    $sums | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding utf8NoBOM
    Get-ChildItem -LiteralPath $Destination -File | Select-Object Name,Length
} finally { Pop-Location }
