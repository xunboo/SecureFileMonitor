#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidatePattern('^\d+(\.\d+)?$')][string]$Altitude = '370030.42',
    [switch]$Load
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $taskRoot "out\x64\$Configuration\SecureFileMonitor.Driver\SecureFileMonitor.sys"
$target = Join-Path $env:SystemRoot 'System32\drivers\SecureFileMonitor.sys'
$serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\SecureFileMonitor'
if (!(Test-Path -LiteralPath $source)) { throw 'Build and sign the driver first.' }
$signature = Get-AuthenticodeSignature -LiteralPath $source
if ($signature.Status -ne 'Valid') { throw "Driver signature is $($signature.Status). Sign the driver and establish certificate trust on the test machine first." }
if ((Test-Path -LiteralPath $serviceKey) -or (Test-Path -LiteralPath $target)) {
    throw 'SecureFileMonitor is already registered or its destination file exists. Use Uninstall-Driver.ps1 before replacing it.'
}
if (!$PSCmdlet.ShouldProcess('SecureFileMonitor', 'Install the demand-start development minifilter')) { return }
$copied = $false
$created = $false
try {
    Copy-Item -LiteralPath $source -Destination $target -ErrorAction Stop
    $copied = $true
    & sc.exe create SecureFileMonitor type= filesys start= demand error= normal binPath= $target depend= FltMgr group= 'FSFilter Activity Monitor' DisplayName= 'Secure File Monitor'
    if ($LASTEXITCODE -ne 0) { throw 'Creating the driver service failed.' }
    $created = $true
    foreach ($suffix in @('Instances', 'Parameters\Instances')) {
        $instances = Join-Path $serviceKey $suffix
        New-Item -Path $instances -Force | Out-Null
        New-ItemProperty -LiteralPath $instances -Name DefaultInstance -Value 'SecureFileMonitor Instance' -PropertyType String -Force | Out-Null
        $instance = Join-Path $instances 'SecureFileMonitor Instance'
        New-Item -Path $instance -Force | Out-Null
        New-ItemProperty -LiteralPath $instance -Name Altitude -Value $Altitude -PropertyType String -Force | Out-Null
        New-ItemProperty -LiteralPath $instance -Name Flags -Value 0 -PropertyType DWord -Force | Out-Null
    }
    if ($Load) {
        & fltmc.exe load SecureFileMonitor
        if ($LASTEXITCODE -ne 0) { throw 'Windows refused to load the driver. Check signature trust, test-signing policy, and the Code Integrity log.' }
    }
} catch {
    if ($created) { & sc.exe delete SecureFileMonitor | Out-Null }
    # This exact file was created by this invocation, and is never a directory.
    if ($copied) { Remove-Item -LiteralPath $target -Force }
    throw
}
Write-Output "Installed $target (demand start, development altitude $Altitude)."
if (!$Load) { Write-Output 'Load when ready: fltmc load SecureFileMonitor' }

