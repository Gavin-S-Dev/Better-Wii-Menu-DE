@echo off
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FORK_VS=%%i"
if not defined FORK_VS exit /b 1
call "%FORK_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "FORK_TOOLS=%~dp0build-win-release\toolchain\msvc\VC\Tools\MSVC\14.51.36231"
set "PATH=%FORK_TOOLS%\bin\Hostx64\x64;%FORK_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%FORK_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%FORK_TOOLS%\include;%INCLUDE%"
set "LIB=%FORK_TOOLS%\lib\x64;%LIB%"
cmake -S "%~dp0." -B "%~dp0build-candidate" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=/EHsc -DCMAKE_LINKER="%FORK_TOOLS%/bin/Hostx64/x64/link.exe" -DCMAKE_AR="%FORK_TOOLS%/bin/Hostx64/x64/lib.exe" -DCMAKE_ASM_MASM_COMPILER="%FORK_TOOLS%/bin/Hostx64/x64/ml64.exe" -DCMAKE_C_COMPILER="%FORK_TOOLS%/bin/Hostx64/x64/cl.exe" -DCMAKE_CXX_COMPILER="%FORK_TOOLS%/bin/Hostx64/x64/cl.exe" -DENABLE_ANALYTICS=OFF -DDISABLE_NLS=ON -DENABLE_AUTOUPDATE=OFF -DDISTRIBUTOR=BetterWiiMenuDE -DUSE_SYSTEM_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=FALSE -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DCMAKE_DISABLE_FIND_PACKAGE_SYSTEMD=TRUE
if errorlevel 1 exit /b 1
if "%1"=="configure" exit /b 0
cmake --build "%~dp0build-candidate" --target dolphin-emu tests --parallel 8

if errorlevel 1 exit /b 1
python "%~dp0Tools/deploy_fork_runtime.py"
