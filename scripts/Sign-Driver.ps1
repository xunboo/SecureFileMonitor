param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$CertificateThumbprint
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$driver = Join-Path $taskRoot "out\x64\$Configuration\SecureFileMonitor.Driver\SecureFileMonitor.sys"
if (!(Test-Path -LiteralPath $driver)) { throw 'Build the driver before signing it.' }
$kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signTool = Get-ChildItem -Path (Join-Path $kit '*\x64\signtool.exe') | Sort-Object FullName -Descending | Select-Object -First 1
if (!$signTool) { throw 'signtool.exe was not found in the Windows SDK.' }
if ($CertificateThumbprint) {
    $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
} else {
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Secure File Monitor Development Test' `
        -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(2)
}
& $signTool.FullName sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $driver
if ($LASTEXITCODE -ne 0) { throw 'Driver signing failed.' }
$publicCertificate = Join-Path (Split-Path -Parent $driver) 'SecureFileMonitor-Test.cer'
Export-Certificate -Cert $certificate -FilePath $publicCertificate -Force | Out-Null
Write-Output "Signed driver: $driver"
Write-Output "Public certificate: $publicCertificate"
Write-Output 'This script does not enable test-signing, change Secure Boot, trust the certificate, install, or load the driver.'
Write-Output 'Prepare your Windows test VM using docs\driver-validation.md before installing.'

