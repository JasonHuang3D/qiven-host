@echo off
setlocal

set "ROOT=%~dp0.."
set "PATCH=%ROOT%\jason-brother.patch"

pushd "%ROOT%"

if not exist "%PATCH%" (
    echo [Qiven] Patch not found:
    echo %PATCH%
    goto :error
)

where git >nul 2>nul
if errorlevel 1 (
    echo [Qiven] Git was not found in PATH.
    goto :error
)

git diff --quiet && git diff --cached --quiet
if errorlevel 1 (
    echo [Qiven] Working tree has tracked changes. Commit or revert them first.
    goto :error
)

echo [Qiven] Checking jason-brother.patch...
git apply --ignore-space-change --check "%PATCH%"
if errorlevel 1 goto :error

echo [Qiven] Applying jason-brother.patch...
git apply --ignore-space-change "%PATCH%"
if errorlevel 1 goto :error

git diff --check
if errorlevel 1 goto :error_changed

echo [Qiven] Formatting C/C++ sources...
call "%ROOT%\tools\format.cmd"
if errorlevel 1 goto :error_changed

call "%ROOT%\tools\format-check.cmd"
if errorlevel 1 goto :error_changed

git diff --check
if errorlevel 1 goto :error_changed

echo [Qiven] Regenerating Visual Studio solution...
call "%ROOT%\tools\gen-vs2022-x64.cmd"
if errorlevel 1 goto :error_changed

del /q "%PATCH%"

popd
echo [Qiven] Patch applied successfully to qiven-host.
exit /b 0

:error
popd
echo.
echo [Qiven] Patch was not applied.
exit /b 1

:error_changed
popd
echo.
echo [Qiven] Working tree changed; inspect git diff before continuing.
exit /b 1
