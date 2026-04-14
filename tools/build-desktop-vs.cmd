@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%\.."

call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo [SwitchBOX] Failed to initialize Visual Studio developer environment.
    popd
    exit /b 1
)

cmake --build build\desktop-vs --target switchbox_desktop --parallel
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
echo [SwitchBOX] Desktop build succeeded:
echo [SwitchBOX]   build\desktop-vs\app\switchbox_desktop.exe

popd
endlocal
