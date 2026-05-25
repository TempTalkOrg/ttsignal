@echo off
:: Build the Windows Node addon and, on success, scan the produced .node
:: for absolute build-host paths or usernames
:: (see scripts/verify-no-sensitive-info.py for the exact pattern set).
::
:: A hit hard-fails this script so a leaky build never silently reaches
:: scripts\upload-release-for-win.bat.
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "ROOT_DIR=%SCRIPT_DIR%\.."
set "BUILD_DIR=%ROOT_DIR%\build\win-x64-release"

call "%BUILD_DIR%\build.bat" %*
if errorlevel 1 (
    echo [build] FAILED: cmake/ninja build returned %ERRORLEVEL% 1>&2
    exit /b %ERRORLEVEL%
)

set "PY="
where python  >nul 2>nul && set "PY=python"
if not defined PY (
    where python3 >nul 2>nul && set "PY=python3"
)
if not defined PY (
    echo WARN: python not found on PATH; skipping sensitive-info verify. 1>&2
    exit /b 0
)

set "ADDON=%ROOT_DIR%\node_modules\ttsignal\dist\build\Release\ttsignal.win32.x64.node"
if not exist "%ADDON%" (
    echo WARN: artifact not found, skipping verify: "%ADDON%" 1>&2
    exit /b 0
)

"%PY%" "%SCRIPT_DIR%\verify-no-sensitive-info.py" "%ADDON%"
if errorlevel 1 (
    echo [build] FAILED: sensitive info detected in addon. See log above. 1>&2
    exit /b 1
)

echo ============================================================
echo [build] addon built and verified clean.
echo ============================================================
endlocal
