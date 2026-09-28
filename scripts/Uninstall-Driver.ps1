#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess)]
param()
$ErrorActionPreference = 'Stop'
$serviceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\SecureFileMonitor'
if (!(Test-Path -LiteralPath $serviceKey)) { Write-Output 'SecureFileMonitor is not installed.'; return }
if (!$PSCmdlet.ShouldProcess('SecureFileMonitor', 'Unload and remove the development minifilter')) { return }
$service = [System.ServiceProcess.ServiceController]::new('SecureFileMonitor')
try {
    if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        & fltmc.exe unload SecureFileMonitor
        if ($LASTEXITCODE -ne 0) { throw 'The driver could not be unloaded. No files have been removed.' }
    }
} finally { $service.Dispose() }
& sc.exe delete SecureFileMonitor
if ($LASTEXITCODE -ne 0) { throw 'Removing the driver registration failed.' }
$target = Join-Path $env:SystemRoot 'System32\drivers\SecureFileMonitor.sys'
if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
Write-Output 'The driver was removed. Application settings, logs, certificates, and Windows signing policy were preserved.'
