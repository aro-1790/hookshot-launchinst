@echo off
rem hookshot-launchinst: generates both version resources, then hands off to nmake.
rem   build.bat [build | clean], or with no arguments for a menu.
setlocal
cd /d "%~dp0"

set "VER_FILE=resource\version.txt"
set "RC_IN=resource\hookshot-launchinst.rc.in"
set "GEN_RC=obj\hookshot-launchinst.gen.rc"
set "VSPF=%ProgramFiles(x86)%"
if not defined VSPF set "VSPF=%ProgramFiles%"
set "VSWHERE=%VSPF%\Microsoft Visual Studio\Installer\vswhere.exe"

rem Called by build.nmake; not a user subcommand.
if /i "%~1"=="--genrc" goto genrc

set "TARGET="
set "VERB="
if /i "%~1"=="build" set "TARGET=all"
if /i "%~1"=="clean" set "TARGET=clean"

if not defined TARGET (
    if "%~1"=="" goto menu
    echo Error: unknown subcommand '%~1'
    echo        Usage: build.bat [build ^| clean]
    exit /b 1
)

rem clean needs no toolchain.
if /i "%TARGET%"=="clean" goto nmake

if defined VCToolsInstallDir (
    echo   toolchain found in the environment
) else (
    echo   toolchain not in the environment - looking for a Visual Studio Developer Command Prompt
    if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat"
)
if not defined VCToolsInstallDir (
    echo Error: no valid Visual Studio toolchain found
    exit /b 1
)
rem build.nmake resolves the host toolset and decides which arches can be built.

:nmake
nmake /NOLOGO /f build.nmake %TARGET%
exit /b %errorlevel%

:genrc
if not exist obj mkdir obj
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=([IO.File]::ReadAllText('%VER_FILE%')).Trim(); $p=$t.TrimStart('v').Split('.'); if($p.Count -lt 3){Write-Host 'Error: resource\version.txt must hold a version like v1.2.3'; exit 1}; $w=$p[0]+','+$p[1]+','+$p[2]+',0'; $t='v'+$t.TrimStart('v'); $s=([IO.File]::ReadAllText('%RC_IN%')).Replace('__WIN_VER__',$w).Replace('__REV__',$t); $m='Version resource: '+$t+' -> '+$w; if((-not (Test-Path '%GEN_RC%')) -or ([IO.File]::ReadAllText('%GEN_RC%') -ne $s)){[IO.File]::WriteAllText('%GEN_RC%',$s,[Text.Encoding]::ASCII)} else {$m=$m+' (unchanged)'}; Write-Host $m"
exit /b %errorlevel%

:menu
cls
echo   1. build
echo   2. clean
echo   3. quit
set "VERB="
choice /C 123 /N /M "Choice: "
if errorlevel 255 exit /b 1
if errorlevel 3 exit /b 0
if errorlevel 2 (set "VERB=clean" & goto chosen)
if errorlevel 1 (set "VERB=build" & goto chosen)
if not defined VERB exit /b 1

:chosen
call "%~f0" %VERB%
if errorlevel 1 goto failed
goto menu

:failed
echo.
echo Press any key to continue...
pause >nul
goto menu
