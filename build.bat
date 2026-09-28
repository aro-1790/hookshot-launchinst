@echo off
rem hookshot-launchinst: generates both version resources, then hands off to nmake.
rem   build.bat [build | clean | rebuild]
rem Needs a registered MSVC toolchain in the environment.
setlocal
cd /d "%~dp0"

set "VER_FILE=resource\version.txt"
set "RC_IN=resource\hookshot-launchinst.rc.in"

rem Called by build.nmake for each architecture; not a user subcommand.
if /i "%~1"=="--genrc" goto genrc

set "TARGET=all"
if /i "%~1"=="build"   set "TARGET=all"
if /i "%~1"=="clean"   set "TARGET=clean"
if /i "%~1"=="rebuild" set "TARGET=rebuild"

rem clean needs no toolchain.
if /i "%TARGET%"=="clean" goto ready

if not defined VCToolsInstallDir (
    echo Error: no MSVC toolchain in the environment.
    echo        Open a Developer Command Prompt, or run your toolchain's register step.
    exit /b 1
)
where nmake >NUL 2>&1
if errorlevel 1 (
    echo Error: nmake is not on PATH. Run this from a Developer Command Prompt.
    exit /b 1
)

rem Both architectures are cross-built, so the host toolset must carry both targets.
set "HOSTARCH="
for %%h in (x64 x86) do if not defined HOSTARCH if exist "%VCToolsInstallDir%bin\Host%%h\x86\cl.exe" if exist "%VCToolsInstallDir%bin\Host%%h\x64\cl.exe" set "HOSTARCH=%%h"
if not defined HOSTARCH (
    echo Error: no host toolset with both targets under "%VCToolsInstallDir%bin\".
    echo        Expected Hostx64 or Hostx86 containing x86 and x64 subdirectories.
    exit /b 1
)
set "HOSTARG=HOSTARCH=%HOSTARCH%"

:ready
call :genrc 32
if errorlevel 1 exit /b 1
call :genrc 64
if errorlevel 1 exit /b 1

nmake /NOLOGO /f build.nmake %HOSTARG% %TARGET%
exit /b %errorlevel%

:genrc
set "ARCH=%~1"
if /i "%~1"=="--genrc" set "ARCH=%~2"
if /i not "%ARCH%"=="32" if /i not "%ARCH%"=="64" (
    echo Error: genrc needs an architecture.
    exit /b 1
)
if not exist obj mkdir obj
powershell -NoProfile -ExecutionPolicy Bypass -Command "$a='%ARCH%'; $t=([IO.File]::ReadAllText('%VER_FILE%')).Trim(); $p=$t.TrimStart('v').Split('.'); if($p.Count -lt 3){Write-Host 'Error: resource\version.txt must hold a version like v1.2.3'; exit 1}; $w=$p[0]+','+$p[1]+','+$p[2]+',0'; $t='v'+$t.TrimStart('v'); $o='obj\hookshot-launchinst'+$a+'.gen.rc'; $s=([IO.File]::ReadAllText('%RC_IN%')).Replace('__WIN_VER__',$w).Replace('__REV__',$t).Replace('__ORIGINAL_FILENAME__','hookshot-launchinst'+$a+'.exe'); if((-not (Test-Path $o)) -or ([IO.File]::ReadAllText($o) -ne $s)){[IO.File]::WriteAllText($o,$s,[Text.Encoding]::ASCII); Write-Host ('Version resource '+$a+': '+$t+' -> '+$w)} else {Write-Host ('Version resource '+$a+': '+$t+' -> '+$w+' (unchanged)')}"
exit /b %errorlevel%
