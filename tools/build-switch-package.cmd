@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%\.."

set "PATH=C:\devkitPro\msys2\usr\bin;%PATH%"
set "DEVKITPRO=/opt/devkitpro"
set "DEVKITA64=/opt/devkitpro/devkitA64"

C:\devkitPro\msys2\usr\bin\cmake.exe --build build\switch --target switchbox_switch_package --parallel
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
echo [SwitchBOX] Switch package build succeeded:
echo [SwitchBOX]   build\releases\...\switch

popd
endlocal
