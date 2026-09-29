@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo   Building Zapret Private Studio (C++ WebView2)
echo ===================================================

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] Failed to load MSVC x64 environment
    exit /b 1
)

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NINJA="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

cd /d "%~dp0"

echo [1/2] Configuring CMake with Ninja...
%CMAKE% -B build -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

echo [2/2] Compiling ZapretPrivate.exe...
%NINJA% -C build
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

echo.
echo ===================================================
echo [SUCCESS] ZapretPrivate.exe compiled successfully!
echo Binary: %~dp0ZapretPrivate.exe
echo ===================================================
