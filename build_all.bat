@echo off
rem ============================================================================
rem  build_all.bat  -  build every sub-project in this repo and show results
rem
rem    1. hid_gif_spiflash_tool          (CMake + MSVC, Windows PC tool)
rem    2. HID_SpiFlash_Writer            (Keil MDK, UV4)
rem    3. GIF_Decode_Player org          (Keil MDK, UV4)
rem    4. GIF_Decode_Player gobal palette(Keil MDK, UV4)
rem
rem  Usage:   build_all.bat          build all projects
rem           build_all.bat clean    rebuild from scratch (CMake dir removed,
rem                                   Keil built with -r rebuild)
rem ============================================================================
setlocal EnableDelayedExpansion

rem --- repo root = folder this script lives in --------------------------------
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

rem --- where to drop per-project build logs -----------------------------------
set "LOGDIR=%ROOT%\build_logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

rem --- optional "clean" rebuild mode ------------------------------------------
set "CLEAN=0"
if /i "%~1"=="clean" set "CLEAN=1"

rem --- locate UV4.exe (Keil MDK) ----------------------------------------------
set "UV4="
for %%P in (
    "C:\Keil_v5\UV4\UV4.exe"
    "C:\Keil\UV4\UV4.exe"
    "D:\Keil_v5\UV4\UV4.exe"
) do if exist %%~P set "UV4=%%~P"
if defined UV4 echo [info] Using UV4: !UV4!
if not defined UV4 echo [warn] UV4.exe not found - Keil projects will be SKIPPED.

rem --- result tracking (indexed variables) ------------------------------------
set "N=0"
set "FAILED=0"

rem ============================================================================
echo.
echo ===============================================================
echo  [1/4] hid_gif_spiflash_tool  (CMake + MSVC)
echo ===============================================================
set "TOOLDIR=%ROOT%\hid_gif_spiflash_tool"
if not exist "%TOOLDIR%\CMakeLists.txt" (
    echo [skip] CMakeLists.txt not found.
    call :record "SKIP" "hid_gif_spiflash_tool"
    goto :keil1
)
if "%CLEAN%"=="1" if exist "%TOOLDIR%\build" rmdir /s /q "%TOOLDIR%\build"
pushd "%TOOLDIR%"
cmake -S . -B build  > "%LOGDIR%\1_hidtool_configure.log" 2>&1
if errorlevel 1 (
    echo [FAIL] CMake configure failed - see build_logs\1_hidtool_configure.log
    call :record "FAIL" "hid_gif_spiflash_tool (configure)"
    set "FAILED=1"
    popd
    goto :keil1
)
cmake --build build --config Release  > "%LOGDIR%\1_hidtool_build.log" 2>&1
if errorlevel 1 (
    echo [FAIL] Build failed - see build_logs\1_hidtool_build.log
    call :record "FAIL" "hid_gif_spiflash_tool"
    set "FAILED=1"
) else (
    echo [ OK ] build\Release\hid_gif_spiflash_tool.exe
    call :record "OK  " "hid_gif_spiflash_tool"
)
popd

:keil1
call :build_keil "2" "HID_SpiFlash_Writer" ^
    "%ROOT%\M3331_Displaying_GIF\SampleCode\ExampleCode\HID_SpiFlash_Writer\Keil\HID_SpiFlash_Writer.uvprojx" ^
    "2_hid_spiflash_writer"

call :build_keil "3" "GIF_Decode_Player org" ^
    "%ROOT%\M3331_Displaying_GIF\SampleCode\ExampleCode\GIF_Decode_Player org\KEIL\GIF_Decode_Player.uvprojx" ^
    "3_gif_player_org"

call :build_keil "4" "GIF_Decode_Player gobal palette" ^
    "%ROOT%\M3331_Displaying_GIF\SampleCode\ExampleCode\GIF_Decode_Player gobal palette\KEIL\GIF_Decode_Player.uvprojx" ^
    "4_gif_player_global_palette"

rem ============================================================================
echo.
echo ===============================================================
echo  Build summary
echo ===============================================================
for /l %%I in (1,1,%N%) do echo   !STATUS_%%I!  !NAME_%%I!
echo ---------------------------------------------------------------
echo  Logs: %LOGDIR%
if "%FAILED%"=="1" (
    echo  Result: one or more projects FAILED.
) else (
    echo  Result: all attempted builds succeeded.
)
echo ===============================================================
endlocal & exit /b %FAILED%

rem ============================================================================
rem  :record  <status>  <name>   -> append to indexed result list
rem ============================================================================
:record
set /a N+=1
set "STATUS_%N%=%~1"
set "NAME_%N%=%~2"
exit /b 0

rem ============================================================================
rem  :build_keil  <step>  <name>  <uvprojx>  <logbase>
rem ============================================================================
:build_keil
set "STEP=%~1"
set "NAME=%~2"
set "PROJ=%~3"
set "LOGBASE=%~4"
echo.
echo ===============================================================
echo  [%STEP%/4] %NAME%  (Keil MDK)
echo ===============================================================
if not defined UV4 (
    echo [skip] UV4.exe not available.
    call :record "SKIP" "%NAME%"
    exit /b 0
)
if not exist "%PROJ%" (
    echo [skip] project not found: %PROJ%
    call :record "SKIP" "%NAME% (no project)"
    exit /b 0
)
rem  -b = build, -r = rebuild all, -j0 = no splash, -o = log file
set "BUILDFLAG=-b"
if "%CLEAN%"=="1" set "BUILDFLAG=-r"
"%UV4%" -j0 %BUILDFLAG% "%PROJ%" -o "%LOGDIR%\%LOGBASE%.log"
set "RC=%ERRORLEVEL%"
rem  UV4 exit codes: 0 = no errors/warnings, 1 = warnings only, >=2 = errors
if %RC% GEQ 2 (
    echo [FAIL] UV4 ExitCode=%RC% - see build_logs\%LOGBASE%.log
    call :record "FAIL" "%NAME% (ExitCode=%RC%)"
    set "FAILED=1"
) else (
    echo [ OK ] UV4 ExitCode=%RC% - see build_logs\%LOGBASE%.log
    call :record "OK  " "%NAME% (ExitCode=%RC%)"
)
exit /b 0
