param([string]$BuildDirectory = '', [string]$EvidenceDirectory = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'out/build/win-static-release' }
if (!$EvidenceDirectory) { $EvidenceDirectory = Join-Path $projectRoot 'out/update-evidence' }
$sourceExecutable = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'mt.exe'
if (!(Test-Path -LiteralPath $sourceExecutable)) { throw '请先构建 mt.exe。' }
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$runRoot = Join-Path $projectRoot ('out/update-verification-' + [guid]::NewGuid().ToString('N'))
$records = [Collections.Generic.List[object]]::new()

function Start-Fixture([string]$Executable, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = Split-Path $Executable -Parent
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    [void]$info.Environment.Remove('QT_PLUGIN_PATH')
    [void]$info.Environment.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($info)
}

foreach ($scenario in @('success', 'invalid-hash', 'locked-executable', 'cancelled')) {
    $root = Join-Path $runRoot $scenario
    $dataRoot = Join-Path $root 'data'
    $stage = Join-Path $root ('.mt-update-' + [guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Path $dataRoot,$stage -Force | Out-Null
    $target = Join-Path $root 'mt.exe'
    $candidate = Join-Path $stage 'candidate.exe'
    $updater = Join-Path $stage 'updater.exe'
    Copy-Item -LiteralPath $sourceExecutable -Destination $target
    Copy-Item -LiteralPath $sourceExecutable -Destination $candidate
    Copy-Item -LiteralPath $sourceExecutable -Destination $updater
    # A harmless PE overlay gives the replacement a different digest while keeping it runnable.
    $stream = [IO.File]::Open($candidate, [IO.FileMode]::Append)
    try { $stream.Write([Text.Encoding]::UTF8.GetBytes('MT isolated update fixture')) } finally { $stream.Dispose() }
    $oldHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    $newHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
    $sentinel = Join-Path $dataRoot 'preserved.txt'
    '更新前的数据' | Set-Content -LiteralPath $sentinel -Encoding utf8NoBOM
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
    $parent = $null
    $helper = $null
    $lockedFile = $null
    try {
        $parent = Start-Fixture $target @('--preview', '--data-dir', $dataRoot, '--quit-after', '5000')
        $plan = [ordered]@{
            target = $target.Replace('\', '/')
            parent = $parent.Id
            created = $parent.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
            sha256 = if ($scenario -eq 'invalid-hash') { '0' * 64 } else { $newHash.ToLowerInvariant() }
            bytes = (Get-Item -LiteralPath $candidate).Length
            dataRoot = $dataRoot
        }
        $planPath = Join-Path $stage 'plan.json'
        $plan | ConvertTo-Json | Set-Content -LiteralPath $planPath -Encoding utf8NoBOM
        if ($scenario -eq 'cancelled') { 'cancelled' | Set-Content -LiteralPath (Join-Path $stage 'cancelled') }
        if ($scenario -eq 'locked-executable') {
            $lockedFile = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        }
        $helper = Start-Fixture $updater @('--preview', '--apply-update', $planPath)
        if (!$helper.WaitForExit(18000)) { throw "更新助手超时：$scenario" }
        $helperExit = $helper.ExitCode
        if (!$parent.WaitForExit(8000) -or $parent.ExitCode -ne 0) { throw "原预览程序未正常退出：$scenario" }
        if ($lockedFile) { $lockedFile.Dispose(); $lockedFile = $null }
        $actualHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($scenario -eq 'success') {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            while (!(Test-Path -LiteralPath (Join-Path $stage 'startup.ok')) -and $watch.ElapsedMilliseconds -lt 7000) {
                Start-Sleep -Milliseconds 100
            }
            if ($helperExit -ne 0 -or $actualHash -ne $newHash -or
                !(Test-Path -LiteralPath (Join-Path $stage 'startup.ok')) -or
                (Get-FileHash -LiteralPath (Join-Path $stage 'previous.exe') -Algorithm SHA256).Hash -ne $oldHash) {
                throw '替换、备份或新程序启动验证失败。'
            }
        } else {
            if ($helperExit -eq 0 -or $actualHash -ne $oldHash -or
                !(Test-Path -LiteralPath (Join-Path $stage 'error.txt'))) { throw "失败保护未生效：$scenario" }
            if ($scenario -eq 'invalid-hash' -and (Test-Path -LiteralPath (Join-Path $stage 'ready'))) {
                throw '损坏文件不应进入安装握手。'
            }
        }
        if ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $sentinelHash) { throw '数据文件被修改。' }
        $records.Add([ordered]@{ scenario = $scenario; passed = $true; helperExit = $helperExit; directory = $root })
        Write-Output "更新集成检查通过：$scenario"
    } finally {
        if ($lockedFile) { $lockedFile.Dispose() }
        foreach ($process in @($helper, $parent)) {
            if ($process) {
                if (!$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(3000) }
                $process.Dispose()
            }
        }
    }
}
$records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'installer-checks.json') -Encoding utf8NoBOM
