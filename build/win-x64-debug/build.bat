@echo off
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set ROOT_DIR=%SCRIPT_DIR%..\..
set SRC_DIR=%ROOT_DIR%\src
set BUILD_DIR=%ROOT_DIR%\build\win-x64-debug

if /i "%~1"=="clean" (
    echo [INFO] Cleaning build directory: %BUILD_DIR%
    for /d %%d in ("%BUILD_DIR%\*") do rmdir /s /q "%%d"
    for %%f in ("%BUILD_DIR%\*") do (
        if /i not "%%~nxf"=="build.bat" del /q "%%f"
    )
    echo [INFO] Clean done.
    exit /b 0
)

echo ============================================
echo  Building ttsignal for Windows x64 (MSVC)
echo ============================================
echo.

REM --- Check Perl and Go availability, then add fallback paths ---
where perl >nul 2>&1
if %errorlevel% neq 0 (
    set "PATH=!PERL_HOME!\bin;!PATH!"
    where perl >nul 2>&1
    if !errorlevel! neq 0 (
        echo [ERROR] Perl not found. Install Perl or set PERL_HOME.
        exit /b 1
    )
)
where go >nul 2>&1
if %errorlevel% neq 0 (
    set "PATH=!GOROOT!\bin;!PATH!"
    where go >nul 2>&1
    if !errorlevel! neq 0 (
        echo [ERROR] Go not found. Install Go or set GOROOT.
        exit /b 1
    )
)

REM --- Check if already in MSVC developer environment ---
where cl >nul 2>&1
if %errorlevel% neq 0 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [ERROR] vswhere not found. Please install Visual Studio 2019 or later.
        exit /b 1
    )

    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"

    if not defined VS_PATH (
        echo [ERROR] Visual Studio with C++ workload not found.
        exit /b 1
    )

    echo [INFO] Found Visual Studio: !VS_PATH!
    call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64
    if errorlevel 1 (
        echo [ERROR] Failed to initialize MSVC x64 environment.
        exit /b 1
    )
) else (
    echo [INFO] MSVC environment already active.
)

REM --- Create build directory ---
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM --- Ensure output directory and copy index.js ---
if not exist "%ROOT_DIR%\node_modules\ttsignal" mkdir "%ROOT_DIR%\node_modules\ttsignal"
copy /Y "%SRC_DIR%\js\index.js" "%ROOT_DIR%\node_modules\ttsignal\" >nul 2>&1

REM --- CMake Configure ---
echo.
echo [INFO] Running CMake configure...
cd /d "%BUILD_DIR%"

cmake -G "Ninja" ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl ^
    "%SRC_DIR%"

if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    exit /b 1
)

REM --- Build ---
echo.
echo [INFO] Building with %NUMBER_OF_PROCESSORS% parallel jobs...
cmake --build . --config Debug -j %NUMBER_OF_PROCESSORS%

if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo ============================================
echo  Build succeeded!
echo  Output: %ROOT_DIR%\node_modules\ttsignal\Debug\
echo ============================================
