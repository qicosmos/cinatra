param(
  [string]$BuildDir = 'build',
  [string]$Config = 'Release',
  [int]$Port = 18080,
  [int]$ServerDurationSec = 120,
  [int]$CaseTimeoutSec = 5,
  [string]$H2SpecExe = '',
  [string[]]$Specs = @(
    'generic/1',
    'generic/2',
    'generic/3',
    'generic/4',
    'generic/5',
    'http2/3.5',
    'http2/4',
    'http2/5.1',
    'http2/5.3.1',
    'http2/5.4.1',
    'http2/5.5',
    'http2/6.1',
    'http2/6.2',
    'http2/6.3',
    'http2/6.4',
    'http2/6.5',
    'http2/6.7',
    'http2/6.8',
    'http2/6.9',
    'http2/6.10',
    'http2/7',
    'http2/8',
    'hpack/2.3.3',
    'hpack/4.2',
    'hpack/5.2',
    'hpack/6.1',
    'hpack/6.3'
  )
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ServerExe = Join-Path $RepoRoot "$BuildDir\example\$Config\http2_conformance_server.exe"
if (-not $H2SpecExe) {
  $H2SpecExe = Join-Path $RepoRoot 'third_party\h2spec\h2spec.exe'
}
$OutputDir = Join-Path $RepoRoot 'third_party\h2spec_isolated'

if (-not (Test-Path $ServerExe)) {
  throw "missing server binary: $ServerExe"
}
if (-not (Test-Path $H2SpecExe)) {
  throw "missing h2spec binary: $H2SpecExe"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Set-Variable -Name H2SpecResultsList -Scope Script `
  -Value ([System.Collections.Generic.List[object]]::new())

function Wait-ServerReady {
  param(
    [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
    [Parameter(Mandatory = $true)][string]$StdoutPath
  )

  for ($i = 0; $i -lt 100; $i++) {
    if ($Process.HasExited) {
      throw "server exited early with code $($Process.ExitCode)"
    }
    if (Test-Path $StdoutPath) {
      $text = Get-Content $StdoutPath -Raw -ErrorAction SilentlyContinue
      if ($text -match 'listening on') {
        return
      }
    }
    Start-Sleep -Milliseconds 50
  }

  throw 'server did not become ready'
}

foreach ($spec in $Specs) {
  $safeName = ($spec -replace '[^A-Za-z0-9._-]', '_')
  $serverOut = Join-Path $env:TEMP "cinatra_${safeName}_server.out"
  $serverErr = Join-Path $env:TEMP "cinatra_${safeName}_server.err"
  $specOut = Join-Path $OutputDir "${safeName}.txt"

  Remove-Item -Force -ErrorAction SilentlyContinue $serverOut, $serverErr, $specOut

  $server = Start-Process -FilePath $ServerExe `
    -ArgumentList '--port', "$Port", '--duration', "$ServerDurationSec" `
    -WorkingDirectory $RepoRoot `
    -RedirectStandardOutput $serverOut `
    -RedirectStandardError $serverErr `
    -WindowStyle Hidden `
    -PassThru

  try {
    Wait-ServerReady -Process $server -StdoutPath $serverOut
    & $H2SpecExe -h 127.0.0.1 -p $Port -t -k -o $CaseTimeoutSec $spec 2>&1 |
      Tee-Object -FilePath $specOut
    $exitCode = $LASTEXITCODE
  }
  finally {
    if ($server -and -not $server.HasExited) {
      Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
      $server.WaitForExit(5000) | Out-Null
    }
  }

  [string]$stderr = ''
  if (Test-Path $serverErr) {
    $stderr = Get-Content $serverErr -Raw -ErrorAction SilentlyContinue
  }

  $script:H2SpecResultsList.Add([pscustomobject]@{
    spec = $spec
    exit_code = $exitCode
    output = $specOut
    server_stderr = ([string]$stderr).Trim()
  })
}

$summaryPath = Join-Path $OutputDir 'summary.txt'
$passCount = @($script:H2SpecResultsList |
  Where-Object { $_.exit_code -eq 0 }).Count
$failures = @($script:H2SpecResultsList |
  Where-Object { $_.exit_code -ne 0 })
$failureCount = @($failures).Count

@(
  "Specs: $($script:H2SpecResultsList.Count)"
  "Passed: $passCount"
  "Failed: $failureCount"
) | Set-Content -Path $summaryPath

if ($failureCount -gt 0) {
  Add-Content -Path $summaryPath -Value ''
  foreach ($failure in $failures) {
    Add-Content -Path $summaryPath -Value "$($failure.spec) -> exit $($failure.exit_code)"
    Add-Content -Path $summaryPath -Value "output: $($failure.output)"
    if ($failure.server_stderr) {
      Add-Content -Path $summaryPath -Value "server stderr: $($failure.server_stderr)"
    }
  }
}

Get-Content $summaryPath
