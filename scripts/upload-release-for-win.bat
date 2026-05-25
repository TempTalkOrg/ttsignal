@echo off
:: ============================================================================
:: scripts/upload-release-for-win.bat
::
:: Publish the Windows Node.js native addon (and the JS loader) to the same
:: GitHub Release that hosts the iOS xcframework + the linux/macos addons.
::
:: Uploaded assets:
::   - node_modules\ttsignal\Release\ttsignal.win32.x64.node
::   - src\js\index.js
::
:: Companion scripts (run from a build host that owns those slices):
::   - scripts/upload-release-for-linux-and-macos.sh — linux + macOS .node files
::   - ios/scripts/release-xcframework.sh           — TTSignal.xcframework zip
::
:: Versioning policy: identical to the bash/iOS variants — the tag is
:: `1.0.YYYYMMDD` where YYYYMMDD is src\cpp\Utils.cpp's last-write date,
:: which matches __DATE__ baked into the SDK at compile time. All three
:: scripts therefore land on the same release entry per build day. The
:: patch component is the date so the string stays valid 3-segment SemVer
:: (SwiftPM and CocoaPods reject 4-segment versions like `1.0.0.YYYYMMDD`).
::
:: Usage:
::   scripts\upload-release-for-win.bat
::   scripts\upload-release-for-win.bat --version 1.0.20260429
::   scripts\upload-release-for-win.bat --repo owner/other-release
::
:: Env overrides (alternative to flags):
::   TTSIGNAL_VERSION  e.g. 1.0.20260429    (default: 1.0.<Utils.cpp mtime YYYYMMDD>)
::   TTSIGNAL_REPO     e.g. owner/ttsignal-xcframework  (default: 3th1UOYgUtJkurSZ/ttsignal-xcframework)
::   TTSIGNAL_TAG      git tag name          (default: same as %TTSIGNAL_VERSION%)
::
:: Requires: `gh` CLI on PATH (winget install GitHub.cli) and PowerShell 5+.
:: ============================================================================
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
:: strip the trailing backslash from %~dp0 so REPO_ROOT joins cleanly
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "REPO_ROOT=%SCRIPT_DIR%\.."

:: -------------------------------------------------------- argument parsing
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--version" (
    set "TTSIGNAL_VERSION=%~2"
    shift & shift & goto parse_args
)
if /I "%~1"=="--repo" (
    set "TTSIGNAL_REPO=%~2"
    shift & shift & goto parse_args
)
if /I "%~1"=="--tag" (
    set "TTSIGNAL_TAG=%~2"
    shift & shift & goto parse_args
)
if /I "%~1"=="-h" goto print_help
if /I "%~1"=="--help" goto print_help
echo unknown argument: %~1 1>&2
goto print_help
:args_done

:: -------------------------------------------------------- defaults
:: Pull mtime of src\cpp\Utils.cpp via PowerShell — same source the bash
:: scripts use, ensuring the tag matches the iOS / linux+macos uploads to
:: the byte. PowerShell is shipped with all supported Windows versions.
set "UTILS_CPP=%REPO_ROOT%\src\cpp\Utils.cpp"
if "%TTSIGNAL_VERSION%"=="" (
    if not exist "%UTILS_CPP%" (
        echo ERROR: cannot derive SDK version: "%UTILS_CPP%" not found, and 1>&2
        echo        no --version / TTSIGNAL_VERSION was supplied. 1>&2
        exit /b 1
    )
    for /f "usebackq tokens=*" %%d in (`powershell -NoProfile -Command "(Get-Item '%UTILS_CPP%').LastWriteTime.ToString('yyyyMMdd')"`) do set "UTILS_MDATE=%%d"
    set "TTSIGNAL_VERSION=1.0.!UTILS_MDATE!"
)
if "%TTSIGNAL_REPO%"==""    set "TTSIGNAL_REPO=3th1UOYgUtJkurSZ/ttsignal-xcframework"
if "%TTSIGNAL_TAG%"==""     set "TTSIGNAL_TAG=%TTSIGNAL_VERSION%"

:: -------------------------------------------------------- asset paths
set "ADDON=%REPO_ROOT%\node_modules\ttsignal\Release\ttsignal.win32.x64.node"
set "INDEX_JS=%REPO_ROOT%\src\js\index.js"

:: -------------------------------------------------------- sanity checks
where gh >nul 2>nul
if errorlevel 1 (
    echo ERROR: 'gh' CLI not found on PATH. Install with: winget install GitHub.cli 1>&2
    exit /b 1
)
gh auth status -h github.com >nul 2>nul
if errorlevel 1 (
    echo ERROR: gh is not authenticated. Run: gh auth login -h github.com 1>&2
    exit /b 1
)
if not exist "%ADDON%" (
    echo ERROR: missing asset: "%ADDON%" 1>&2
    echo Hint: build the Windows addon first, e.g. scripts\build-for-win.bat 1>&2
    exit /b 1
)
if not exist "%INDEX_JS%" (
    echo ERROR: missing asset: "%INDEX_JS%" 1>&2
    exit /b 1
)

:: -------------------------------------------------------- summary
echo ============================================================
echo [release] version : %TTSIGNAL_VERSION%
echo [release] tag     : %TTSIGNAL_TAG%
echo [release] repo    : %TTSIGNAL_REPO%
echo [release] assets  :
echo                     %ADDON%
echo                     %INDEX_JS%
echo ============================================================

:: -------------------------------------------------------- create / upload
:: If the release already exists (e.g. iOS or linux/macos uploaded first),
:: append the Windows assets via --clobber. Otherwise create it.
set "NOTES_FILE=%TEMP%\ttsignal-xcframework-notes-%RANDOM%.md"
> "%NOTES_FILE%" echo TTSignal release `%TTSIGNAL_VERSION%`.
>>"%NOTES_FILE%" echo.
>>"%NOTES_FILE%" echo Assets uploaded by `scripts/upload-release-for-win.bat`:
>>"%NOTES_FILE%" echo.
>>"%NOTES_FILE%" echo - `ttsignal.win32.x64.node` — Windows x86_64 Node addon
>>"%NOTES_FILE%" echo - `index.js`                — Node.js loader (selects the addon by `process.platform` + `process.arch`)
>>"%NOTES_FILE%" echo.
>>"%NOTES_FILE%" echo Companion uploads (separate scripts):
>>"%NOTES_FILE%" echo - `scripts/upload-release-for-linux-and-macos.sh` — linux + macOS `.node` files
>>"%NOTES_FILE%" echo - `ios/scripts/release-xcframework.sh`            — `ttsignal-swift.zip` (SPM binaryTarget)

gh release view "%TTSIGNAL_TAG%" --repo "%TTSIGNAL_REPO%" >nul 2>nul
if errorlevel 1 (
    echo [release] creating release %TTSIGNAL_TAG% on %TTSIGNAL_REPO%
    gh release create "%TTSIGNAL_TAG%" "%ADDON%" "%INDEX_JS%" ^
        --repo "%TTSIGNAL_REPO%" ^
        --title "TTSignal %TTSIGNAL_VERSION%" ^
        --notes-file "%NOTES_FILE%"
    set "RC=!ERRORLEVEL!"
) else (
    echo [release] tag %TTSIGNAL_TAG% already exists — uploading with --clobber
    gh release upload "%TTSIGNAL_TAG%" "%ADDON%" "%INDEX_JS%" ^
        --repo "%TTSIGNAL_REPO%" --clobber
    set "RC=!ERRORLEVEL!"
)

del /q "%NOTES_FILE%" >nul 2>nul
if not "%RC%"=="0" (
    echo [release] FAILED: gh exited with %RC% 1>&2
    exit /b %RC%
)

echo ============================================================
echo [release] OK — released to https://github.com/%TTSIGNAL_REPO%/releases/tag/%TTSIGNAL_TAG%
echo ============================================================
endlocal
goto :eof

:print_help
:: print the leading "::" comment block so `--help` prints the same usage
:: text we keep at the top of this file
for /f "usebackq tokens=* delims=" %%l in ("%~f0") do (
    set "line=%%l"
    if "!line:~0,3!"==":: " (
        echo !line:~3!
    ) else if "!line:~0,2!"=="::" (
        echo.
    ) else (
        goto :eof
    )
)
exit /b 0
