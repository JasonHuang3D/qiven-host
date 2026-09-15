@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0resolve-toolchain.cmd"
if errorlevel 1 exit /b 1
pushd "%ROOT%" || exit /b 1
"%QIVEN_CMAKE%" --preset vs2022-x64
if errorlevel 1 (
    popd
    exit /b 1
)
if not exist "%ROOT%\build\vs2022-x64\qiven-host.sln" (
    echo [Qiven] Expected solution was not generated: qiven-host.sln
    popd
    exit /b 1
)
popd
echo [Qiven] Visual Studio solution generated.
exit /b 0
