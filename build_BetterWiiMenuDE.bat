@echo off
title Better Wii Menu DE - Build Script
echo ============================================
echo   Better Wii Menu DE - Automated Builder
echo ============================================
echo.

:: Check for Visual Studio
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [!] Visual Studio C++ compiler not found.
    echo.
    echo You need Visual Studio 2022 with "Desktop development with C++" installed.
    echo Download free: https://visualstudio.microsoft.com/vs/community/
    echo.
    echo After installing, run this script from "x64 Native Tools Command Prompt for VS 2022"
    echo  (find it in Start Menu under Visual Studio 2022)
    echo.
    pause
    exit /b 1
)

:: Check for CMake
where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo [!] CMake not found. It should come with Visual Studio.
    echo     Re-run Visual Studio Installer and make sure CMake is checked.
    pause
    exit /b 1
)

:: Check for Git
where git >nul 2>nul
if %errorlevel% neq 0 (
    echo [!] Git not found. Download from https://git-scm.com/download/win
    pause
    exit /b 1
)

echo [1/4] Initializing submodules...
git submodule update --init --recursive
if %errorlevel% neq 0 (
    echo [!] Failed to initialize submodules.
    pause
    exit /b 1
)

echo.
echo [2/4] Configuring build (this may take a minute)...
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DENABLE_AUTOUPDATE=OFF -DDISABLE_NLS=ON
if %errorlevel% neq 0 (
    echo [!] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo [3/4] Building Better Wii Menu DE (this will take a while)...
cmake --build build --config Release --parallel
if %errorlevel% neq 0 (
    echo [!] Build failed.
    pause
    exit /b 1
)

echo.
echo [4/4] Checking for NSIS to build installer...
if exist "C:\Program Files (x86)\NSIS\makensis.exe" (
    echo Building installer...
    if not exist "Binary\x64" mkdir "Binary\x64"
    xcopy /E /I /Y "build\Binaries\Release" "Binary\x64"
    "C:\Program Files (x86)\NSIS\makensis.exe" /DDOLPHIN_ARCH=x64 /DPRODUCT_VERSION=1.0 Source\Installer\Installer.nsi
    if %errorlevel% equ 0 (
        echo.
        echo ============================================
        echo   INSTALLER BUILT SUCCESSFULLY!
        echo   Find it at: Source\Installer\BetterWiiMenuDE-x64-1.0.exe
        echo ============================================
    )
) else (
    echo NSIS not found - skipping installer creation.
    echo You can still run the app directly.
)

echo.
echo ============================================
echo   BUILD COMPLETE!
echo   Run it from: build\Binaries\Release\BetterWiiMenuDE.exe
echo ============================================
echo.
pause
