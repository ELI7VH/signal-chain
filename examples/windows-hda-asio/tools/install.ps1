# install.ps1 -- Install HDA Direct ASIO (requires admin)
#
# Usage: Start-Process powershell -Verb RunAs -ArgumentList '-File','C:\Users\elija\signal-chain\examples\windows-hda-asio\tools\install.ps1'

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=== Installing HDA Direct ASIO ===" -ForegroundColor Cyan
Write-Host ""

# Step 1: Check for test signing
$testSigning = bcdedit /enum | Select-String "testsigning\s+Yes"
if (-not $testSigning) {
    Write-Host "WARNING: Test signing is NOT enabled." -ForegroundColor Yellow
    Write-Host "The kernel driver requires test signing mode."
    Write-Host ""
    Write-Host "To enable (requires reboot):"
    Write-Host "  bcdedit /set testsigning on"
    Write-Host ""
    $response = Read-Host "Enable test signing now? (y/n)"
    if ($response -eq 'y') {
        bcdedit /set testsigning on
        Write-Host "Test signing enabled. REBOOT REQUIRED before driver will load." -ForegroundColor Yellow
    } else {
        Write-Host "Skipping. Driver won't load until test signing is enabled." -ForegroundColor Yellow
    }
}

# Step 2: Create test certificate and sign driver
$driverPath = Join-Path $scriptDir "hda_bridge.sys"
if (Test-Path $driverPath) {
    Write-Host ""
    Write-Host "Signing driver with test certificate..." -ForegroundColor Cyan

    # Create test cert if it doesn't exist
    $cert = Get-ChildItem -Path Cert:\LocalMachine\Root | Where-Object { $_.Subject -like "*HdaBridge*" }
    if (-not $cert) {
        Write-Host "Creating test certificate..."
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=HdaBridge Test" -CertStoreLocation Cert:\LocalMachine\My
        # Export and import to Trusted Root
        $certPath = Join-Path $scriptDir "HdaBridge.cer"
        Export-Certificate -Cert $cert -FilePath $certPath | Out-Null
        Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        Write-Host "Test certificate created and trusted."
    }

    # Sign with signtool (from WDK) or with Set-AuthenticodeSignature
    $signingCert = Get-ChildItem -Path Cert:\LocalMachine\My -CodeSigningCert | Where-Object { $_.Subject -like "*HdaBridge*" } | Select-Object -First 1
    if ($signingCert) {
        Set-AuthenticodeSignature -FilePath $driverPath -Certificate $signingCert -TimestampServer "http://timestamp.digicert.com"
        Write-Host "Driver signed." -ForegroundColor Green
    } else {
        Write-Host "WARNING: Could not sign driver (no code signing cert found)." -ForegroundColor Yellow
    }
} else {
    Write-Host "WARNING: hda_bridge.sys not found in tools directory." -ForegroundColor Yellow
    Write-Host "Build the driver first: build_driver.bat"
}

# Step 3: Install kernel driver
if (Test-Path $driverPath) {
    Write-Host ""
    Write-Host "Installing kernel driver..." -ForegroundColor Cyan

    # Remove existing service if present
    $existing = Get-Service -Name "HdaAsioBridge" -ErrorAction SilentlyContinue
    if ($existing) {
        if ($existing.Status -eq "Running") {
            Stop-Service -Name "HdaAsioBridge" -Force
        }
        sc.exe delete HdaAsioBridge | Out-Null
        Start-Sleep -Seconds 1
    }

    # Create the service
    sc.exe create HdaAsioBridge type= kernel binPath= "$driverPath"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Driver service created." -ForegroundColor Green

        # Try to start it
        sc.exe start HdaAsioBridge
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Driver STARTED." -ForegroundColor Green
        } else {
            Write-Host "Driver could not start (reboot may be needed for test signing)." -ForegroundColor Yellow
        }
    } else {
        Write-Host "Failed to create driver service." -ForegroundColor Red
    }
}

# Step 4: Register ASIO DLL
$dllPath = Join-Path $scriptDir "hda_asio.dll"
if (Test-Path $dllPath) {
    Write-Host ""
    Write-Host "Registering ASIO DLL..." -ForegroundColor Cyan
    regsvr32 /s "$dllPath"
    Write-Host "ASIO DLL registered." -ForegroundColor Green
} else {
    Write-Host "WARNING: hda_asio.dll not found. Build it first: build_dll.bat" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== Installation Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "If driver didn't start, reboot and run: sc start HdaAsioBridge"
Write-Host "Test with: asio_loopback.exe"
Write-Host "Use in FL Studio: Options > Audio Settings > select 'HDA Direct ASIO'"
