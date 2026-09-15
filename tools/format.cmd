@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0resolve-toolchain.cmd"
if errorlevel 1 exit /b 1
where git >nul 2>nul || (echo [Qiven] Git was not found in PATH.& exit /b 1)
pushd "%ROOT%" || exit /b 1
echo [Qiven] Formatting tracked C/C++ sources...
for /f "delims=" %%F in ('git ls-files -- "*.c" "*.cc" "*.cpp" "*.cxx" "*.h" "*.hh" "*.hpp" "*.hxx" "*.inl" "*.ipp" "*.tpp"') do (
    "%QIVEN_CLANG_FORMAT%" -i --style=file "%%F" || (popd & exit /b 1)
)
popd
echo [Qiven] Formatting complete.
