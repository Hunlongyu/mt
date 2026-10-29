param([string]$BuildDirectory = 'out/build/win-static-release', [string[]]$TestCases = @())
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
$exe = Join-Path $buildRoot 'mt_lock_integration.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Build mt_lock_integration first.' }
$result = Join-Path $buildRoot 'test-results/screen-lock-native.txt'
$env:MT_RUN_LOCK_INTEGRATION = '1'
try {
    # Dedicated test process only. The parent keeps a hard deadline even if the
    # test GUI or its own hook thread fails; daily mt/reference tools are untouched.
    $testProcess = Start-Process -FilePath $exe -ArgumentList (@('-o', "`"$result,txt`"") + $TestCases) -WindowStyle Hidden -PassThru
    if (!$testProcess.WaitForExit(45000)) {
        $testProcess.Kill()
        $testProcess.WaitForExit()
        throw 'Blackout desktop tests timed out; the isolated test process was stopped.'
    }
    Get-Content -LiteralPath $result
    if ($testProcess.ExitCode -ne 0) { throw "Blackout tests failed: $($testProcess.ExitCode)" }
} finally {
    Remove-Item Env:MT_RUN_LOCK_INTEGRATION -ErrorAction SilentlyContinue
}
