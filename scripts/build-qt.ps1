param(
    [string]$InstallRoot = 'C:\mt-deps\qt',
    [string]$WorkRoot = 'C:\mt-deps\build',
    [string]$ArchiveRoot = 'C:\mt-deps\archives',
    [int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/msvc.ps1"
Import-MsvcEnvironment
$manifest = Get-Content -Raw "$PSScriptRoot/dependencies.json" | ConvertFrom-Json
foreach ($directory in @($InstallRoot, $WorkRoot, $ArchiveRoot)) {
    if ($directory -match '[&|<>%\r\n"]') { throw '构建目录包含不支持的字符。' }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
foreach ($module in $manifest.archives) {
    $archive = Join-Path $ArchiveRoot $module.file
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri ($manifest.baseUrl + $module.file) -OutFile ($archive + '.part') -MaximumRetryCount 3 -TimeoutSec 300
        if ((Get-FileHash -LiteralPath ($archive + '.part') -Algorithm SHA256).Hash -ine $module.sha256) { throw "Qt 源码校验失败：$($module.file)" }
        Move-Item -LiteralPath ($archive + '.part') -Destination $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $module.sha256) { throw "Qt 源码校验失败：$($module.file)" }
    $source = Join-Path $WorkRoot ($module.file -replace '\.tar\.xz$', '')
    if (!(Test-Path -LiteralPath $source)) {
        & python "$PSScriptRoot/extract-qt.py" $archive $WorkRoot
        Assert-NativeSuccess '解压 Qt'
    }
    # The precompiled Chinese translation is embedded; retain its corresponding source.
    if ($module.name -eq 'qttranslations') { continue }
    $build = Join-Path $WorkRoot ($module.name + '-build')
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    if ($module.name -eq 'qtbase') {
        Push-Location $build
        try {
            & (Join-Path $source 'configure.bat') -prefix $InstallRoot -static -static-runtime -release -opensource -confirm-license -nomake examples -nomake tests -no-opengl -no-icu -no-openssl -schannel -- -DFEATURE_sql_odbc=OFF -DFEATURE_sql_psql=OFF -DFEATURE_sql_mysql=OFF
            Assert-NativeSuccess '配置 Qt Base'
        } finally { Pop-Location }
    } else {
        & (Join-Path $InstallRoot 'bin/qt-cmake.bat') -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$InstallRoot" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF
        Assert-NativeSuccess '配置 Qt SVG'
    }
    & cmake --build $build --parallel $Parallel
    Assert-NativeSuccess "构建 $($module.name)"
    & cmake --install $build
    Assert-NativeSuccess "安装 $($module.name)"
}
Copy-Item -LiteralPath "$PSScriptRoot/dependencies.json" -Destination (Join-Path $InstallRoot 'mt-dependencies.json')
