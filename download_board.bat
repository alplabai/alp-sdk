@echo off
setlocal

:: Define the URL and the destination directory
set "URL=https://raw.githubusercontent.com/alifsemi/alif_vscode-template/main/board/board.h"
set "DEST_DIR=board-config\alif-ensemble-e7-gen2"

:: Create the destination directory if it doesn't exist
if not exist "%DEST_DIR%" (
    mkdir "%DEST_DIR%"
)

:: Define the destination file path
set "DEST_FILE=%DEST_DIR%\board.h"

:: Download the file
powershell -Command "Invoke-WebRequest -Uri %URL% -OutFile %DEST_FILE%"

:: Verify the download
if exist "%DEST_FILE%" (
    echo File downloaded successfully to %DEST_FILE%
) else (
    echo Failed to download the file
)

endlocal
pause