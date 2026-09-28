param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$test = Join-Path $taskRoot "out\x64\$Configuration\SecureFileMonitor.Tests\SecureFileMonitor.Tests.exe"
if (!(Test-Path -LiteralPath $test)) { throw 'Build the app/tests first using scripts\Build.ps1 -AppOnly.' }
$scratch = Join-Path $taskRoot ('out\test-results\' + [Guid]::NewGuid().ToString('N'))
& $test $scratch
if ($LASTEXITCODE -ne 0) { throw 'Native core tests failed.' }
Write-Output "Test artifacts: $scratch"
