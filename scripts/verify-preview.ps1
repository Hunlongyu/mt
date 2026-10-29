param([string]$BuildDirectory = '', [string]$EvidenceDirectory = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'out\build\win-static-release' }
if (!$EvidenceDirectory) { $EvidenceDirectory = Join-Path $projectRoot 'out\native-evidence' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
$sourceExecutable = Join-Path $BuildDirectory 'mt.exe'
if (!(Test-Path -LiteralPath $sourceExecutable)) { throw 'Build mt.exe first.' }
[IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
$runRoot = Join-Path $projectRoot ('out\preview-verification-' + [Guid]::NewGuid().ToString('N'))
$binaryRoot = Join-Path $runRoot 'binary'
$dataRoot = Join-Path $runRoot 'data'
[IO.Directory]::CreateDirectory($binaryRoot) | Out-Null
$executable = Join-Path $binaryRoot 'mt.exe'
Copy-Item -LiteralPath $sourceExecutable -Destination $executable

function Start-IsolatedPreview([string[]]$AdditionalArguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $executable
    $info.WorkingDirectory = $binaryRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    [void]$info.Environment.Remove('QT_PLUGIN_PATH')
    [void]$info.Environment.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
    foreach ($argument in (@('--preview', '--data-dir', $dataRoot) + $AdditionalArguments)) {
        $info.ArgumentList.Add($argument)
    }
    return [Diagnostics.Process]::Start($info)
}

$primary = $null
$secondary = $null
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    $primary = Start-IsolatedPreview -AdditionalArguments @('--evidence', $EvidenceDirectory, '--quit-after', '4500')
    # Concurrent startup exercises the mutex before the primary finishes initializing its UI.
    $secondary = Start-IsolatedPreview -AdditionalArguments @('--evidence', $EvidenceDirectory, '--quit-after', '4500')
    $startupWatch = [Diagnostics.Stopwatch]::StartNew()
    while (!$primary.HasExited -and !$secondary.HasExited -and $startupWatch.ElapsedMilliseconds -lt 3500) {
        Start-Sleep -Milliseconds 50
    }
    if ($primary.HasExited -eq $secondary.HasExited) { throw 'Concurrent startup did not leave exactly one running instance.' }
    $winner = if ($primary.HasExited) { $secondary } else { $primary }
    $loser = if ($primary.HasExited) { $primary } else { $secondary }
    $loserCode = $loser.ExitCode
    $winnerAlive = !$winner.HasExited
    if (!$winner.WaitForExit(9000)) { throw 'The preview failed to shut down within the bounded wait.' }
    $winnerCode = $winner.ExitCode
    $record = [ordered]@{
        timestamp = (Get-Date).ToString('o')
        mode = 'same-machine standalone-directory; system-only child PATH; synthetic data'
        executable = $executable
        dataRoot = $dataRoot
        filesInBinaryDirectory = @(Get-ChildItem -LiteralPath $binaryRoot -File | ForEach-Object Name)
        winningPid = $winner.Id
        duplicatePid = $loser.Id
        duplicateExitCode = $loserCode
        winnerAliveAfterDuplicate = $winnerAlive
        winnerExitCode = $winnerCode
        elapsedMs = $watch.ElapsedMilliseconds
        cleanMachineDeploymentVerified = $false
    }
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'standalone-check.json') -Encoding utf8
    if ($winnerCode -ne 0 -or $loserCode -ne 0 -or !$winnerAlive) { throw 'Standalone or singleton checks failed; inspect the evidence.' }
} finally {
    foreach ($process in @($secondary, $primary)) {
        if ($null -ne $process) {
            if (!$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(3000) }
            $process.Dispose()
        }
    }
}

$binary = Get-Item -LiteralPath $sourceExecutable
[ordered]@{
    bytes = $binary.Length
    mebibytes = [Math]::Round($binary.Length / 1MB, 2)
    sha256 = (Get-FileHash -LiteralPath $sourceExecutable -Algorithm SHA256).Hash
    productVersion = $binary.VersionInfo.ProductVersion
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'binary.json') -Encoding utf8
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$dumpbin = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -Last 1
if (!$dumpbin) { throw 'dumpbin was not found through vswhere.' }
$imports = & $dumpbin /nologo /dependents $sourceExecutable
if ($LASTEXITCODE -ne 0) { throw 'PE dependency inspection failed.' }
$imports | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'pe-imports.txt') -Encoding utf8
if ($imports -match '(?i)\b(Qt\d[^\s]*|MSVCP\d[^\s]*|VCRUNTIME\d[^\s]*)\.dll') {
    throw 'An unexpected Qt or VC runtime DLL dependency was detected.'
}
Write-Output "Preview, singleton, normal exit and PE import checks passed: $EvidenceDirectory"
