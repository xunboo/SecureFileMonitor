param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$AppOnly
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer / vswhere.exe is missing.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) {
    foreach ($cand in @('C:\Program Files\Microsoft Visual Studio\18\Community', 'C:\Program Files\Microsoft Visual Studio\2022\Community')) {
        if (Test-Path -LiteralPath (Join-Path $cand 'MSBuild\Current\Bin\MSBuild.exe')) {
            $vsRoot = $cand
            break
        }
    }
}
if (!$vsRoot) { throw 'Install Visual Studio with Desktop development with C++.' }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe' }
$vcTargets = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'MSBuild\Microsoft\VC') -Directory |
    Where-Object Name -Match '^v\d+$' | Sort-Object Name -Descending | Select-Object -First 1
$projects = if ($AppOnly) { @('sdk\procmonsdk\procmonsdk.vcxproj', 'app\SecureFileMonitor.App.vcxproj', 'tests\SecureFileMonitor.Tests.vcxproj', 'tools\IoProbe.vcxproj') } else { @('SecureFileMonitor.sln') }
foreach ($project in $projects) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $msbuild
    $info.WorkingDirectory = $taskRoot
    $info.UseShellExecute = $false
    $arguments = @((Join-Path $taskRoot $project), '/m:1', '/nr:false', "/p:Configuration=$Configuration", '/p:Platform=x64', '/v:minimal', '/nologo')
    $info.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    # Avoid inherited VS 2022 overrides when selecting VS 2026, and normalize
    # duplicate environment-key casing sometimes present in automated hosts.
    $childEnvironment = $info.EnvironmentVariables
    if ($PSVersionTable.PSVersion.Major -ge 6) { $childEnvironment = $info.Environment }
    $childEnvironment.Clear()
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        if ($seen.Add([string]$entry.Key)) { $childEnvironment[[string]$entry.Key] = [string]$entry.Value }
    }
    $childEnvironment['VCTargetsPath'] = $vcTargets.FullName + '\'
    $childEnvironment.Remove('VSINSTALLDIR') | Out-Null
    $childEnvironment.Remove('VisualStudioVersion') | Out-Null
    Write-Output "Building $project ($Configuration | x64) with $vsRoot"
    $process = [System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "MSBuild failed with exit code $($process.ExitCode)." }
}
