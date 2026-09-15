@echo off
setlocal
if not defined QIVEN_TOOLCHAIN_ROOT set "QIVEN_TOOLCHAIN_ROOT=%~dp0..\..\qiven-toolchain-win"
for %%I in ("%QIVEN_TOOLCHAIN_ROOT%") do set "QIVEN_TOOLCHAIN_ROOT=%%~fI"
set "QIVEN_CLANG_FORMAT=%QIVEN_TOOLCHAIN_ROOT%\bin\clang-format.exe"
set "QIVEN_CMAKE=%QIVEN_TOOLCHAIN_ROOT%\cmake\bin\cmake.exe"
if not exist "%QIVEN_CLANG_FORMAT%" (
    echo [Qiven] clang-format was not found: %QIVEN_CLANG_FORMAT%
    exit /b 1
)
if not exist "%QIVEN_CMAKE%" (
    echo [Qiven] CMake was not found: %QIVEN_CMAKE%
    exit /b 1
)
"%QIVEN_CLANG_FORMAT%" --version 2>nul | findstr /c:"19.1.7" >nul
if errorlevel 1 (
    echo [Qiven] Expected clang-format 19.1.7.
    exit /b 1
)
"%QIVEN_CMAKE%" --version 2>nul | findstr /b /c:"cmake version 4.4.3" >nul
if errorlevel 1 (
    echo [Qiven] Expected CMake 4.4.3.
    exit /b 1
)
endlocal & set "QIVEN_TOOLCHAIN_ROOT=%QIVEN_TOOLCHAIN_ROOT%" & set "QIVEN_CLANG_FORMAT=%QIVEN_CLANG_FORMAT%" & set "QIVEN_CMAKE=%QIVEN_CMAKE%"
exit /b 0
