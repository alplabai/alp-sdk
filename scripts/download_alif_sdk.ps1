#!/usr/bin/env pwsh
# ============================================================================
# Alif Ensemble SDK Downloader (PowerShell)
# ============================================================================
# This script downloads the required Alif Ensemble SDK files for ALP SDK
# 
# Requirements:
#   - Internet connection
#   - PowerShell 5.1+ or PowerShell Core 7+
#
# Usage:
#   Windows:  .\scripts\download_alif_sdk.ps1
#   Linux:    pwsh ./scripts/download_alif_sdk.ps1
#
# ============================================================================

param(
    [switch]$Force,
    [string]$Destination = "vendor/alif/sdk"
)

# Function to download file with retry
function Download-File {
    param(
        [string]$Url,
        [string]$OutFile,
        [string]$Description
    )
    
    $maxRetries = 3
    $retryCount = 0
    $success = $false
    
    while (-not $success -and $retryCount -lt $maxRetries) {
        try {
            Write-Host "  Downloading $Description..." -NoNewline
            
            # Check if file exists
            if (Test-Path $OutFile) {
                if ($Force) {
                    Remove-Item $OutFile -Force
                } else {
                    Write-Host " [SKIP - exists]" -ForegroundColor Yellow
                    return $true
                }
            }
            
            # Download with progress
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $Url -OutFile $OutFile -ErrorAction Stop
            $ProgressPreference = 'Continue'
            
            Write-Host " [OK]" -ForegroundColor Green
            $success = $true
            return $true
        }
        catch {
            $retryCount++
            if ($retryCount -lt $maxRetries) {
                Write-Host " [RETRY $retryCount/$maxRetries]" -ForegroundColor Yellow
                Start-Sleep -Seconds 2
            } else {
                Write-Host " [FAILED]" -ForegroundColor Red
                Write-Host "    Error: $($_.Exception.Message)" -ForegroundColor Red
                return $false
            }
        }
    }
    
    return $false
}

# Main script
Write-Host ""
Write-Host "============================================================================" -ForegroundColor Cyan
Write-Host "  Alif Ensemble SDK Downloader for ALP SDK" -ForegroundColor Cyan
Write-Host "============================================================================" -ForegroundColor Cyan
Write-Host ""

# Create directory structure
Write-Host "[1/6] Creating directory structure..."
$dirs = @(
    $Destination,
    "$Destination/drivers",
    "$Destination/CMSIS",
    "$Destination/include"
)

foreach ($dir in $dirs) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        Write-Host "  Created: $dir" -ForegroundColor Green
    }
}
Write-Host ""

# Define repository URLs
$alifRepo = "https://raw.githubusercontent.com/alifsemi/alif_ensemble-cmsis-dfp/main"
$cmsisRepo = "https://raw.githubusercontent.com/ARM-software/CMSIS_5/develop"

# Download CMSIS Driver headers
Write-Host "[2/6] Downloading CMSIS Driver headers..."
$cmsisFiles = @{
    "Driver_Common.h" = "$cmsisRepo/CMSIS/Driver/Include/Driver_Common.h"
    "Driver_SPI.h" = "$cmsisRepo/CMSIS/Driver/Include/Driver_SPI.h"
    "Driver_USART.h" = "$cmsisRepo/CMSIS/Driver/Include/Driver_USART.h"
    "Driver_I2C.h" = "$cmsisRepo/CMSIS/Driver/Include/Driver_I2C.h"
    "Driver_GPIO.h" = "$cmsisRepo/CMSIS/Driver/Include/Driver_GPIO.h"
}

$cmsisSuccess = 0
foreach ($file in $cmsisFiles.Keys) {
    $result = Download-File -Url $cmsisFiles[$file] -OutFile "$Destination/CMSIS/$file" -Description $file
    if ($result) { $cmsisSuccess++ }
}
Write-Host ""

# Download Alif board configuration
Write-Host "[3/6] Downloading Alif board configuration..."
$boardFiles = @{
    "board.h" = "$alifRepo/Boards/DevKit-e7/board.h"
}

$boardSuccess = 0
foreach ($file in $boardFiles.Keys) {
    $result = Download-File -Url $boardFiles[$file] -OutFile "$Destination/$file" -Description $file
    if ($result) { $boardSuccess++ }
}
Write-Host ""

# Download Alif device headers
Write-Host "[4/6] Downloading Alif device headers..."
$deviceFiles = @{
    "system_M55_HE.h" = "$alifRepo/Device/M55_HE/Include/system_M55_HE.h"
    "system_M55_HP.h" = "$alifRepo/Device/M55_HP/Include/system_M55_HP.h"
}

$deviceSuccess = 0
foreach ($file in $deviceFiles.Keys) {
    $result = Download-File -Url $deviceFiles[$file] -OutFile "$Destination/include/$file" -Description $file
    if ($result) { $deviceSuccess++ }
}
Write-Host ""

# Download Driver_SPI implementation stub
Write-Host "[5/6] Creating driver stubs..."
$driverStub = @"
/**
 * Alif SPI Driver Stub
 * 
 * This is a placeholder. For full functionality, download the complete
 * Alif Ensemble SDK from: https://alifsemi.com/support/software-tools/ensemble/
 * 
 * The actual driver implementation is provided in the Alif CMSIS-DFP pack.
 */

#ifndef DRIVER_SPI_ALIF_H
#define DRIVER_SPI_ALIF_H

#include "Driver_SPI.h"

// External driver instances (provided by Alif SDK)
extern ARM_DRIVER_SPI Driver_SPI0;
extern ARM_DRIVER_SPI Driver_SPI1;

#endif // DRIVER_SPI_ALIF_H
"@

$driverStub | Out-File -FilePath "$Destination/drivers/Driver_SPI_Alif.h" -Encoding UTF8
Write-Host "  Created driver stub: Driver_SPI_Alif.h" -ForegroundColor Green
Write-Host ""

# Create README
Write-Host "[6/6] Creating documentation..."
$readme = @"
# Alif Ensemble SDK

Downloaded from: https://github.com/alifsemi/alif_ensemble-cmsis-dfp
Date: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")

## Contents

- CMSIS Driver headers (SPI, USART, I2C, GPIO)
- Alif board configuration
- Device headers
- Driver stubs

## Downloaded Files

CMSIS Headers: $cmsisSuccess / $($cmsisFiles.Count)
Board Config: $boardSuccess / $($boardFiles.Count)
Device Headers: $deviceSuccess / $($deviceFiles.Count)

## Full SDK Installation

For production use, download the complete Alif Ensemble SDK:

### Option 1: Official Website
1. Visit: https://alifsemi.com/support/software-tools/ensemble/
2. Download the full SDK package
3. Extract to: vendor/alif/sdk
4. Update ALIF_SDK_PATH in CMake

### Option 2: CMSIS Pack Manager
``````bash
cpackget init https://www.keil.com/pack/index.pidx
cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/download/v1.3.0/AlifSemiconductor.Ensemble.1.3.0.pack
``````

### Option 3: Git Clone
``````bash
git clone https://github.com/alifsemi/alif_ensemble-cmsis-dfp.git vendor/alif/cmsis-dfp
``````

## Build with ALP SDK

``````bash
# Configure with Alif support
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=vendor/alif/sdk

# Build
cmake --build build

# Run example (requires Alif E7 hardware)
./build/bin/alif_spi_example
``````

## Important Notes

⚠️ The files downloaded by this script are MINIMAL headers for compilation.
⚠️ For actual hardware execution, you MUST install the full Alif SDK.
⚠️ The full SDK includes:
   - Complete driver implementations
   - Linker scripts
   - Startup code
   - Device configuration
   - Flash programming tools

## Support

- Alif Website: https://alifsemi.com
- Documentation: https://alifsemi.com/support/
- GitHub: https://github.com/alifsemi
- Forum: https://community.alifsemi.com

"@

$readme | Out-File -FilePath "$Destination/README.md" -Encoding UTF8
Write-Host "  Created: README.md" -ForegroundColor Green
Write-Host ""

# Summary
$totalFiles = $cmsisFiles.Count + $boardFiles.Count + $deviceFiles.Count
$totalSuccess = $cmsisSuccess + $boardSuccess + $deviceSuccess

Write-Host "============================================================================" -ForegroundColor Cyan
Write-Host "  Download Summary" -ForegroundColor Cyan
Write-Host "============================================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  CMSIS Headers:    $cmsisSuccess / $($cmsisFiles.Count)" -ForegroundColor $(if ($cmsisSuccess -eq $cmsisFiles.Count) { "Green" } else { "Yellow" })
Write-Host "  Board Config:     $boardSuccess / $($boardFiles.Count)" -ForegroundColor $(if ($boardSuccess -eq $boardFiles.Count) { "Green" } else { "Yellow" })
Write-Host "  Device Headers:   $deviceSuccess / $($deviceFiles.Count)" -ForegroundColor $(if ($deviceSuccess -eq $deviceFiles.Count) { "Green" } else { "Yellow" })
Write-Host "  ---" 
Write-Host "  Total:            $totalSuccess / $totalFiles files" -ForegroundColor $(if ($totalSuccess -eq $totalFiles) { "Green" } else { "Yellow" })
Write-Host ""
Write-Host "  Location: $Destination" -ForegroundColor Cyan
Write-Host ""

if ($totalSuccess -eq $totalFiles) {
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "  SUCCESS! Alif SDK files downloaded" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Next steps:" -ForegroundColor White
    Write-Host "  1. Open project in VS Code" -ForegroundColor White
    Write-Host "  2. Build: cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=$Destination" -ForegroundColor White
    Write-Host "  3. Compile: cmake --build build" -ForegroundColor White
    Write-Host ""
    Write-Host "  Note: For actual hardware, download full SDK from:" -ForegroundColor Yellow
    Write-Host "        https://alifsemi.com/support/software-tools/ensemble/" -ForegroundColor Yellow
    Write-Host ""
} else {
    Write-Host "============================================================================" -ForegroundColor Yellow
    Write-Host "  PARTIAL SUCCESS - Some files could not be downloaded" -ForegroundColor Yellow
    Write-Host "============================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  This may be due to:" -ForegroundColor White
    Write-Host "  - Internet connection issues" -ForegroundColor White
    Write-Host "  - Changed repository structure" -ForegroundColor White
    Write-Host "  - GitHub API rate limiting" -ForegroundColor White
    Write-Host ""
    Write-Host "  For full SDK, visit:" -ForegroundColor White
    Write-Host "  https://alifsemi.com/support/software-tools/ensemble/" -ForegroundColor Cyan
    Write-Host ""
}

Write-Host "============================================================================" -ForegroundColor Cyan
Write-Host ""
