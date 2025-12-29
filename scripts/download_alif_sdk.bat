@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: Alif Ensemble SDK Downloader
:: ============================================================================
:: This script downloads the required Alif Ensemble SDK files for ALP SDK
:: 
:: Requirements:
::   - Internet connection
::   - PowerShell (Windows 7+)
::
:: Usage:
::   scripts\download_alif_sdk.bat
::
:: ============================================================================

echo.
echo ============================================================================
echo   Alif Ensemble SDK Downloader for ALP SDK
echo ============================================================================
echo.

:: Define destination directory
set "VENDOR_DIR=vendor\alif"
set "SDK_DIR=%VENDOR_DIR%\sdk"
set "DRIVERS_DIR=%SDK_DIR%\drivers"
set "CMSIS_DIR=%SDK_DIR%\CMSIS"

:: Create directory structure
echo [1/5] Creating directory structure...
if not exist "%VENDOR_DIR%" mkdir "%VENDOR_DIR%"
if not exist "%SDK_DIR%" mkdir "%SDK_DIR%"
if not exist "%DRIVERS_DIR%" mkdir "%DRIVERS_DIR%"
if not exist "%CMSIS_DIR%" mkdir "%CMSIS_DIR%"
echo   Created: %SDK_DIR%
echo.

:: Define Alif GitHub repository base URL
set "ALIF_REPO=https://raw.githubusercontent.com/alifsemi/alif_ensemble-cmsis-dfp/main"
set "CMSIS_REPO=https://raw.githubusercontent.com/ARM-software/CMSIS_5/develop"

:: Download CMSIS Driver headers
echo [2/5] Downloading CMSIS Driver headers...
set "CMSIS_FILES=Driver_Common.h Driver_SPI.h Driver_USART.h Driver_I2C.h Driver_GPIO.h"

for %%F in (%CMSIS_FILES%) do (
    echo   Downloading %%F...
    powershell -Command "try { Invoke-WebRequest -Uri '%CMSIS_REPO%/CMSIS/Driver/Include/%%F' -OutFile '%CMSIS_DIR%\%%F' -ErrorAction Stop; Write-Host '    Success: %%F' -ForegroundColor Green } catch { Write-Host '    Warning: Failed to download %%F' -ForegroundColor Yellow }"
)
echo.

:: Download Alif board configuration
echo [3/5] Downloading Alif board configuration...
echo   Downloading board.h...
powershell -Command "try { Invoke-WebRequest -Uri '%ALIF_REPO%/Boards/DevKit-e7/board.h' -OutFile '%SDK_DIR%\board.h' -ErrorAction Stop; Write-Host '    Success: board.h' -ForegroundColor Green } catch { Write-Host '    Warning: Failed to download board.h' -ForegroundColor Yellow }"
echo.

:: Download Alif device headers (example)
echo [4/5] Downloading Alif device headers...
set "ALIF_HEADERS=system_M55_HE.h system_M55_HP.h"

for %%F in (%ALIF_HEADERS%) do (
    echo   Downloading %%F...
    powershell -Command "try { Invoke-WebRequest -Uri '%ALIF_REPO%/Device/M55_HE/Include/%%F' -OutFile '%SDK_DIR%\%%F' -ErrorAction Stop; Write-Host '    Success: %%F' -ForegroundColor Green } catch { Write-Host '    Warning: Could not download %%F' -ForegroundColor Yellow }"
)
echo.

:: Create SDK info file
echo [5/5] Creating SDK info file...
(
echo # Alif Ensemble SDK
echo.
echo Downloaded from: https://github.com/alifsemi/alif_ensemble-cmsis-dfp
echo Date: %date% %time%
echo.
echo ## Contents:
echo - CMSIS Driver headers ^(SPI, USART, I2C, GPIO^)
echo - Alif board configuration
echo - Device headers
echo.
echo ## Full SDK Installation:
echo For production use, download the complete Alif Ensemble SDK:
echo 1. Visit: https://alifsemi.com/support/software-tools/ensemble/
echo 2. Download the full SDK package
echo 3. Extract to: vendor/alif/sdk
echo 4. Update ALIF_SDK_PATH in CMake
echo.
echo ## Alternative: CMSIS Pack
echo Install via CMSIS Pack Manager:
echo ```
echo cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/latest
echo ```
) > "%SDK_DIR%\README.txt"
echo   Created: %SDK_DIR%\README.txt
echo.

:: Verify downloads
echo ============================================================================
echo   Download Summary
echo ============================================================================
echo.

set "SUCCESS_COUNT=0"
set "TOTAL_COUNT=0"

for %%F in (%CMSIS_FILES%) do (
    set /a TOTAL_COUNT+=1
    if exist "%CMSIS_DIR%\%%F" (
        set /a SUCCESS_COUNT+=1
        echo   [OK] %%F
    ) else (
        echo   [MISSING] %%F
    )
)

if exist "%SDK_DIR%\board.h" (
    set /a TOTAL_COUNT+=1
    set /a SUCCESS_COUNT+=1
    echo   [OK] board.h
) else (
    set /a TOTAL_COUNT+=1
    echo   [MISSING] board.h
)

echo.
echo   Downloaded: !SUCCESS_COUNT! / !TOTAL_COUNT! files
echo   Location: %SDK_DIR%
echo.

if !SUCCESS_COUNT! EQU !TOTAL_COUNT! (
    echo ============================================================================
    echo   SUCCESS! Alif SDK files downloaded
    echo ============================================================================
    echo.
    echo   Next steps:
    echo   1. Open project in VS Code
    echo   2. Build with: cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=vendor/alif/sdk
    echo   3. Run: cmake --build build
    echo.
) else (
    echo ============================================================================
    echo   PARTIAL SUCCESS - Some files could not be downloaded
    echo ============================================================================
    echo.
    echo   This may be due to:
    echo   - Internet connection issues
    echo   - Changed repository structure
    echo   - GitHub API rate limiting
    echo.
    echo   For full SDK, visit: https://alifsemi.com/support/software-tools/ensemble/
    echo.
)

echo ============================================================================
echo.

endlocal
pause
