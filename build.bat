@echo off
setlocal

set ROOT=%~dp0.
set SRC=%ROOT%\src
set SDK=%ROOT%\external\FarManager\plugins\common\unicode
set SDK2=%ROOT%\external\FarManager\plugins\common
set DIFF_ENGINE=%ROOT%\external\comparePlus\src\Engine

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
)
if %ERRORLEVEL% NEQ 0 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
)

if not exist "%ROOT%\build" mkdir "%ROOT%\build"

set CL_FLAGS=/W3 /O2 /EHsc /std:c++17 /DNDEBUG /D_WINDOWS /D_USRDLL /D_WINDLL /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%SDK%" /I"%SDK2%" /I"%DIFF_ENGINE%"

echo === Building FarCompare.dll (x64) ===
cl.exe %CL_FLAGS% /Fe"%ROOT%\build\FarCompare.dll" "%SRC%\FarCompare.cpp" /Fo"%ROOT%\build\\" /link /DLL /DEF:"%SRC%\FarCompare.def" /OUT:"%ROOT%\build\FarCompare.dll"

if %ERRORLEVEL% EQU 0 (
    copy "%SRC%\FarCompEng.lng" "%ROOT%\build\" >nul
    copy "%SRC%\FarCompHun.lng" "%ROOT%\build\" >nul
    copy "%ROOT%\macros\FarCompare_keys.lua" "%ROOT%\build\" >nul
    echo OK: build\FarCompare.dll
) else (
    echo BUILD FAILED
    exit /b 1
)
