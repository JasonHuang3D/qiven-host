@echo off
setlocal EnableExtensions
set "QIVEN_OPERATOR_PY=%~dp0qiven.py"

if defined QIVEN_PYTHON goto validate_configured_python

:try_python
where python >nul 2>nul
if errorlevel 1 goto try_py_launcher
python -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if errorlevel 1 goto try_py_launcher
goto use_python

:try_py_launcher
where py >nul 2>nul
if errorlevel 1 goto no_supported_python
py -3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if errorlevel 1 goto no_supported_python
goto use_py_launcher

:validate_configured_python
"%QIVEN_PYTHON%" -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if errorlevel 1 goto configured_python_invalid
goto use_configured_python

:configured_python_invalid
echo [FAIL] QIVEN_PYTHON must point to Python 3.9 or newer: %QIVEN_PYTHON%
exit /b 2

:no_supported_python
echo [FAIL] Qiven Operator requires Python 3.9 or newer. Set QIVEN_PYTHON or make a supported python available on PATH.
exit /b 2

:use_configured_python
"%QIVEN_PYTHON%" "%QIVEN_OPERATOR_PY%" %*
set "QIVEN_EXIT_CODE=%errorlevel%"
goto finish

:use_python
python "%QIVEN_OPERATOR_PY%" %*
set "QIVEN_EXIT_CODE=%errorlevel%"
goto finish

:use_py_launcher
py -3 "%QIVEN_OPERATOR_PY%" %*
set "QIVEN_EXIT_CODE=%errorlevel%"

:finish
exit /b %QIVEN_EXIT_CODE%
