function Import-MsvcEnvironment {
    $locator = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $installation = & $locator -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw '未找到 MSVC C++ 工具链。' }
    $setup = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ('mt-msvc-' + [guid]::NewGuid().ToString('N') + '.cmd')
    try {
        @('@echo off', 'chcp 65001 >nul', ('call "{0}" >nul' -f $setup), 'if errorlevel 1 exit /b %errorlevel%', 'set VSLANG=1033', 'set') | Set-Content -LiteralPath $temporary -Encoding ascii
        $environmentLines = & cmd.exe /d /c $temporary
        if ($LASTEXITCODE -ne 0) { throw 'MSVC 环境初始化失败。' }
        foreach ($line in $environmentLines) {
            if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
        }
    } finally { Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue }
}

function Assert-NativeSuccess([string]$step) {
    if ($LASTEXITCODE -ne 0) { throw "$step 失败，退出码 $LASTEXITCODE" }
}
