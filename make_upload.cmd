@echo off
rem ===========================================================================
rem  make_upload.cmd - assemble the minimum GitHub upload for Grim Numbers.
rem
rem  Copies the sources, docs, licences and the built binaries into .\upload,
rem  leaving out build intermediates, debug symbols, the reference clones and
rem  the raw analysis dumps.
rem
rem  Usage:  make_upload.cmd [output-directory]
rem          default output is .\upload
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BIN=%ROOT%\build\Release"

if "%~1"=="" (set "OUT=%ROOT%\upload") else (set "OUT=%~f1")
if /i "%OUT%"=="%ROOT%" goto :bad_out
for %%D in (src include docs tools config third_party build) do (
    if /i "%OUT%"=="%ROOT%\%%D" goto :bad_out
)

rem --- what never travels ----------------------------------------------------
set "XF=*.pdb *.ilk *.exp *.lib *.obj *.user *.suo *.aps *.tlog *.log"
set "XD=.git .vs build out upload reference analysis docs __pycache__"

echo.
echo   Grim Numbers - staging upload
echo   from : %ROOT%
echo   to   : %OUT%
echo.

rem --- binaries must exist before we claim to ship them -----------------------
set "MISSING="
for %%F in (dinput8.dll grimnumbers_app.exe grimnumbers_client.exe grimnumbers_pipe_smoke.exe) do (
    if not exist "%BIN%\%%F" set "MISSING=!MISSING! %%F"
)
if defined MISSING (
    echo   ERROR: not built yet:!MISSING!
    echo       Run: cmake --build build --config Release
    echo.
    goto :fail
)

if exist "%OUT%" (
    echo   clearing %OUT%
    rmdir /s /q "%OUT%" || goto :fail
)
mkdir "%OUT%" || goto :fail

rem --- repository root files --------------------------------------------------
echo   root files
for %%F in (README.md CMakeLists.txt .gitignore make_upload.cmd
            LICENSE_GrimNumbers.txt LICENSE_GDDPSMeter.txt LICENSE_MinHook.txt) do (
    copy /y "%ROOT%\%%F" "%OUT%\%%F" >nul || goto :fail
)

rem --- source tree -------------------------------------------------------------
call :sync include        || goto :fail
call :sync src            || goto :fail
call :sync tools          || goto :fail
call :sync config         || goto :fail
call :sync public_docs docs || goto :fail
call :sync third_party    || goto :fail

rem --- built artefacts ---------------------------------------------------------
rem  MinHook is BSD-2-Clause: a binary redistribution has to carry its notice,
rem  so the licences ship next to the DLL, not only at the repository root.
echo   Release
mkdir "%OUT%\Release" || goto :fail
for %%F in (dinput8.dll grimnumbers_app.exe grimnumbers_client.exe
            grimnumbers_pipe_smoke.exe) do (
    copy /y "%BIN%\%%F" "%OUT%\Release\%%F" >nul || goto :fail
)
for %%F in (overlay.ini elements.ini) do (
    if exist "%BIN%\%%F" copy /y "%BIN%\%%F" "%OUT%\Release\%%F" >nul
)
for %%F in (LICENSE_GrimNumbers.txt LICENSE_GDDPSMeter.txt LICENSE_MinHook.txt) do (
    copy /y "%ROOT%\%%F" "%OUT%\Release\%%F" >nul || goto :fail
)

rem --- report -------------------------------------------------------------------
set "COUNT=0"
for /r "%OUT%" %%F in (*) do set /a COUNT+=1
echo.
echo   staged %COUNT% files in %OUT%
echo.
echo   Excluded: docs\ (the Grim Dawn analysis notes, held back), build
echo   intermediates, *.pdb debug symbols, reference\ and analysis\.
echo   public_docs\ ships in their place, as docs\.
echo.
echo   This tree has no .git - it is a plain copy, so no commit history and
echo   no author identity travel with it. On the machine that holds the
echo   credentials, start a fresh repository:
echo.
echo       git init
echo       git config user.name  "your-github-handle"
echo       git config user.email "your-github-noreply-address"
echo       git add .
echo       git commit
echo.
echo   Set user.name and user.email BEFORE the first commit, or the global
echo   identity on that machine is what gets recorded. The README credits
echo   Claude, so keep the trailer on that commit:
echo       Co-Authored-By: Claude Opus 5 ^<noreply@anthropic.com^>
echo.
endlocal
exit /b 0

rem ------------------------------------------------------------------------------
:sync
rem  %1 = source directory, %2 = destination name (defaults to %1)
set "_SRC=%~1"
if "%~2"=="" (set "_DST=%~1") else (set "_DST=%~2")
if "%_SRC%"=="%_DST%" (echo   %_DST%) else (echo   %_DST%  [from %_SRC%\])
robocopy "%ROOT%\%_SRC%" "%OUT%\%_DST%" /E /NFL /NDL /NJH /NJS /NP /XF %XF% /XD %XD% >nul
if errorlevel 8 exit /b 1
exit /b 0

:bad_out
echo   ERROR: refusing to use "%OUT%" as the output directory.
echo.
:fail
echo   FAILED
endlocal
exit /b 1
