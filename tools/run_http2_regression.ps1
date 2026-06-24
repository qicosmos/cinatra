param(
    [string]$BuildDir = "build_http2_test",
    [string]$Config = "Debug",
    [int]$Repeat = 1,
    [int]$ChunkSize = 40,
    [int]$TestTimeoutSec = 180,
    [int]$ExampleTimeoutSec = 60,
    [switch]$SkipBuild,
    [switch]$RunExample
)

$ErrorActionPreference = "Stop"

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [AllowEmptyString()][string]$Arguments = "",
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$TimeoutSec,
        [string]$StdoutPath = "",
        [string]$StderrPath = "",
        [switch]$RedirectOutput
    )

    $startArgs = @{
        FilePath = $FilePath
        WorkingDirectory = $WorkingDirectory
        PassThru = $true
    }
    if ($Arguments) {
        $startArgs.ArgumentList = $Arguments
    }
    if ($RedirectOutput) {
        $startArgs.WindowStyle = "Hidden"
        if ($StdoutPath) {
            $startArgs.RedirectStandardOutput = $StdoutPath
        }
        if ($StderrPath) {
            $startArgs.RedirectStandardError = $StderrPath
        }
    }
    else {
        $startArgs.NoNewWindow = $true
    }

    $process = Start-Process @startArgs

    if (-not $process.WaitForExit($TimeoutSec * 1000)) {
        try {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        catch {
        }
        $process.WaitForExit(5000) | Out-Null
        throw "process timed out after ${TimeoutSec}s: $FilePath $Arguments"
    }

    $process.WaitForExit()
    $process.Refresh()
    return $process.ExitCode
}

function Replay-LogTail {
    param(
        [string]$Label,
        [string]$Path,
        [int]$Tail = 160
    )
    if (Test-Path $Path) {
        $text = Get-Content $Path -Raw -ErrorAction SilentlyContinue
        if ($text) {
            Write-Host "[$Label]"
            Get-Content $Path -Tail $Tail
        }
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$testExe = Join-Path $repoRoot "$BuildDir\tests\$Config\test_http2.exe"
$exampleExe = Join-Path $repoRoot "$BuildDir\example\$Config\http2_example.exe"

Push-Location $repoRoot
try {
    if (-not $SkipBuild) {
        Write-Host "[http2] building test_http2 ($Config)"
        cmake --build $BuildDir --target test_http2 --config $Config
        if ($LASTEXITCODE -ne 0) {
            throw "build failed for test_http2"
        }
    }

    if (-not (Test-Path $testExe)) {
        throw "missing test binary: $testExe"
    }

    $caseList = & $testExe --list-test-cases --no-colors=1
    if ($LASTEXITCODE -ne 0) {
        throw "failed to list test_http2 cases"
    }
    $testCaseCount = @($caseList | Where-Object {
        $_ -and $_ -notmatch '^\[doctest\]' -and $_ -notmatch '^=+'
    }).Count
    if ($testCaseCount -le 0) {
        throw "test_http2 did not report any test cases"
    }

    for ($i = 1; $i -le $Repeat; $i++) {
        Write-Host "[http2] test run $i/$Repeat ($testCaseCount cases)"
        for ($first = 1; $first -le $testCaseCount; $first += $ChunkSize) {
            $last = [Math]::Min($first + $ChunkSize - 1, $testCaseCount)
            Write-Host "[http2] test cases $first-$last"
            $testLog = [System.IO.Path]::GetTempFileName()
            $stdoutFile = [System.IO.Path]::GetTempFileName()
            $stderrFile = [System.IO.Path]::GetTempFileName()
            try {
                $args = "--first=$first --last=$last --no-colors=1 --duration=1 --out=`"$testLog`""
                $exitCode = Invoke-HiddenProcess `
                    -FilePath $testExe `
                    -Arguments $args `
                    -WorkingDirectory $repoRoot `
                    -TimeoutSec $TestTimeoutSec `
                    -StdoutPath $stdoutFile `
                    -StderrPath $stderrFile `
                    -RedirectOutput
                $testOutput = Get-Content $testLog -Raw -ErrorAction SilentlyContinue
                if (($null -ne $exitCode -and $exitCode -ne 0) -or
                    $testOutput -notmatch "Status: SUCCESS") {
                    Replay-LogTail "test_http2 doctest" $testLog
                    Replay-LogTail "test_http2 stdout" $stdoutFile
                    Replay-LogTail "test_http2 stderr" $stderrFile
                    throw "test_http2 failed on iteration $i, cases $first-$last"
                }
            }
            catch {
                Replay-LogTail "test_http2 doctest" $testLog
                Replay-LogTail "test_http2 stdout" $stdoutFile
                Replay-LogTail "test_http2 stderr" $stderrFile
                throw
            }
            finally {
                Remove-Item $testLog, $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($RunExample) {
        if (-not $SkipBuild) {
            Write-Host "[http2] building http2_example ($Config)"
            cmake --build $BuildDir --target http2_example --config $Config
            if ($LASTEXITCODE -ne 0) {
                throw "build failed for http2_example"
            }
        }

        if (-not (Test-Path $exampleExe)) {
            throw "missing example binary: $exampleExe"
        }

        Write-Host "[http2] running http2_example"
        $stdoutFile = [System.IO.Path]::GetTempFileName()
        $stderrFile = [System.IO.Path]::GetTempFileName()
        try {
            $exitCode = Invoke-HiddenProcess `
                -FilePath $exampleExe `
                -WorkingDirectory $repoRoot `
                -TimeoutSec $ExampleTimeoutSec `
                -StdoutPath $stdoutFile `
                -StderrPath $stderrFile `
                -RedirectOutput
            if ($null -ne $exitCode -and $exitCode -ne 0) {
                Replay-LogTail "http2_example stdout" $stdoutFile
                Replay-LogTail "http2_example stderr" $stderrFile
                throw "http2_example failed"
            }
        }
        catch {
            Replay-LogTail "http2_example stdout" $stdoutFile
            Replay-LogTail "http2_example stderr" $stderrFile
            throw
        }
        finally {
            Remove-Item $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "[http2] regression OK"
}
finally {
    Pop-Location
}
