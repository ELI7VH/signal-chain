# uninstall.ps1 -- Remove HDA Direct ASIO (requires admin)
#
# Usage: Start-Process powershell -Verb RunAs -ArgumentList '-File','C:\Users\elija\signal-chain\examples\windows-hda-asio\tools\uninstall.ps1'

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=== Uninstalling HDA Direct ASIO ===" -ForegroundColor Cyan

# Stop and remove driver
$svc = Get-Service -Name "HdaAsioBridge" -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -eq "Running") {
        Write-Host "Stopping driver..."
        Stop-Service -Name "HdaAsioBridge" -Force
    }
    Write-Host "Removing driver service..."
    sc.exe delete HdaAsioBridge | Out-Null
    Write-Host "Driver removed." -ForegroundColor Green
} else {
    Write-Host "Driver not installed (skipping)."
}

# Unregister ASIO DLL
$dllPath = Join-Path $scriptDir "hda_asio.dll"
if (Test-Path $dllPath) {
    Write-Host "Unregistering ASIO DLL..."
    regsvr32 /u /s "$dllPath"
    Write-Host "ASIO DLL unregistered." -ForegroundColor Green
}

# Remove test certificate
$certs = Get-ChildItem -Path Cert:\LocalMachine\Root | Where-Object { $_.Subject -like "*HdaBridge*" }
foreach ($cert in $certs) {
    Write-Host "Removing test certificate..."
    Remove-Item $cert.PSPath -Force
}

Write-Host ""
Write-Host "=== Uninstall Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "To disable test signing: bcdedit /set testsigning off"
