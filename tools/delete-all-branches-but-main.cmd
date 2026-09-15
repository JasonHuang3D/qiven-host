@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0.."
set "LOCAL_CANDIDATES=%TEMP%\qiven-local-branches-%RANDOM%-%RANDOM%.txt"
set "REMOTE_CANDIDATES=%TEMP%\qiven-remote-branches-%RANDOM%-%RANDOM%.txt"

where git >nul 2>nul
if errorlevel 1 (
    echo [Qiven] Git was not found in PATH.
    goto :error_before_pushd
)

pushd "%ROOT%"
if errorlevel 1 (
    echo [Qiven] Could not enter the repository root:
    echo %ROOT%
    goto :error_before_pushd
)

for /f "delims=" %%R in ('git rev-parse --show-toplevel 2^>nul') do set "GIT_ROOT=%%R"
if not defined GIT_ROOT (
    echo [Qiven] The script directory is not inside a valid Git work tree.
    goto :error
)

for %%R in ("%ROOT%") do set "EXPECTED_ROOT=%%~fR"
for %%R in ("!GIT_ROOT!") do set "GIT_ROOT=%%~fR"
if /I not "!GIT_ROOT!"=="!EXPECTED_ROOT!" (
    echo [Qiven] Refusing to operate outside the expected repository.
    echo [Qiven] Script repository: !EXPECTED_ROOT!
    echo [Qiven] Git work tree:    !GIT_ROOT!
    goto :error
)

git remote get-url origin >nul 2>nul
if errorlevel 1 (
    echo [Qiven] Remote origin does not exist.
    goto :error
)

git diff --quiet
if errorlevel 1 (
    echo [Qiven] Working tree has tracked changes. Commit or revert them first.
    goto :error
)

git diff --cached --quiet
if errorlevel 1 (
    echo [Qiven] Index has staged changes. Commit or revert them first.
    goto :error
)

echo [Qiven] Switching to main...
git switch main
if errorlevel 1 goto :error

echo [Qiven] Synchronizing main and pruning origin...
git fetch origin --prune
if errorlevel 1 goto :error

git pull --ff-only origin main
if errorlevel 1 goto :error

git fetch origin --prune
if errorlevel 1 goto :error

git show-ref --verify --quiet refs/heads/main
if errorlevel 1 (
    echo [Qiven] Local main does not exist after synchronization.
    goto :error
)

git show-ref --verify --quiet refs/remotes/origin/main
if errorlevel 1 (
    echo [Qiven] Remote-tracking ref origin/main does not exist after synchronization.
    goto :error
)

type nul >"%LOCAL_CANDIDATES%"
type nul >"%REMOTE_CANDIDATES%"

for /f "delims=" %%B in ('git for-each-ref --format^="%%(refname)" refs/heads/') do (
    if /I not "%%B"=="refs/heads/main" echo %%B>>"%LOCAL_CANDIDATES%"
)

for /f "delims=" %%R in ('git for-each-ref --format^="%%(refname)" refs/remotes/origin/') do (
    git symbolic-ref -q "%%R" >nul 2>nul
    if errorlevel 1 (
        set "REMOTE_REF=%%R"
        if /I "!REMOTE_REF:~0,20!"=="refs/remotes/origin/" (
            set "REMOTE_BRANCH=!REMOTE_REF:~20!"
            if defined REMOTE_BRANCH if /I not "!REMOTE_BRANCH!"=="main" if /I not "!REMOTE_BRANCH!"=="HEAD" (
                echo !REMOTE_REF!>>"%REMOTE_CANDIDATES%"
            )
        )
    )
)

echo [Qiven] Local branches scheduled for deletion:
for /f "usebackq delims=" %%B in ("%LOCAL_CANDIDATES%") do echo [Qiven]   %%B
for %%F in ("%LOCAL_CANDIDATES%") do if %%~zF==0 echo [Qiven]   ^(none^)

echo [Qiven] Remote branches scheduled for deletion:
for /f "usebackq delims=" %%R in ("%REMOTE_CANDIDATES%") do echo [Qiven]   %%R
for %%F in ("%REMOTE_CANDIDATES%") do if %%~zF==0 echo [Qiven]   ^(none^)

echo [Qiven] Running all-or-nothing safety preflight...
set "BLOCKED=0"

for /f "usebackq delims=" %%B in ("%LOCAL_CANDIDATES%") do (
    git merge-base --is-ancestor "%%B" refs/heads/main >nul 2>nul
    if errorlevel 1 (
        echo [Qiven] BLOCKED unmerged local branch: %%B
        set "BLOCKED=1"
    )
)

for /f "usebackq delims=" %%R in ("%REMOTE_CANDIDATES%") do (
    git merge-base --is-ancestor "%%R" refs/remotes/origin/main >nul 2>nul
    if errorlevel 1 (
        echo [Qiven] BLOCKED unmerged remote branch: %%R
        set "BLOCKED=1"
    )
)

if "!BLOCKED!"=="1" (
    echo [Qiven] Safety preflight failed. No branches were deleted.
    goto :error
)

echo [Qiven] Safety preflight passed. Deleting scheduled branches...
for /f "usebackq delims=" %%B in ("%LOCAL_CANDIDATES%") do (
    set "LOCAL_REF=%%B"
    set "LOCAL_BRANCH=!LOCAL_REF:~11!"
    git branch -d -- "!LOCAL_BRANCH!"
    if errorlevel 1 goto :error
    echo [Qiven] Deleted local branch: !LOCAL_BRANCH!
)

for /f "usebackq delims=" %%R in ("%REMOTE_CANDIDATES%") do (
    set "REMOTE_REF=%%R"
    if /I not "!REMOTE_REF:~0,20!"=="refs/remotes/origin/" (
        echo [Qiven] Invalid remote ref encountered after preflight: !REMOTE_REF!
        goto :error
    )
    set "REMOTE_BRANCH=!REMOTE_REF:~20!"
    if not defined REMOTE_BRANCH goto :invalid_remote_branch
    if /I "!REMOTE_BRANCH!"=="main" goto :invalid_remote_branch
    if /I "!REMOTE_BRANCH!"=="HEAD" goto :invalid_remote_branch
    git push origin --delete "!REMOTE_BRANCH!"
    if errorlevel 1 goto :error
    echo [Qiven] Deleted remote branch: !REMOTE_BRANCH!
)

echo [Qiven] Performing final origin prune...
git fetch origin --prune
if errorlevel 1 goto :error

del /q "%LOCAL_CANDIDATES%" "%REMOTE_CANDIDATES%" >nul 2>nul
popd
echo [Qiven] Branch cleanup completed successfully.
echo [Qiven] Only local main and remote branch origin/main remain; symbolic remote refs were preserved.
exit /b 0

:invalid_remote_branch
echo [Qiven] Refusing to delete protected or invalid remote branch: !REMOTE_BRANCH!

:error
del /q "%LOCAL_CANDIDATES%" "%REMOTE_CANDIDATES%" >nul 2>nul
popd
echo [Qiven] Branch cleanup failed.
exit /b 1

:error_before_pushd
del /q "%LOCAL_CANDIDATES%" "%REMOTE_CANDIDATES%" >nul 2>nul
echo [Qiven] Branch cleanup failed.
exit /b 1
