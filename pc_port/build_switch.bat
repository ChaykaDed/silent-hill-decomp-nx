@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
set PATH=%PATH%;C:\devkitPro\tools\bin
set DEVKITPRO=C:\devkitPro

set BUILD_DIR=%~dp0build_switch

if "%1"=="clean" goto clean
if "%1"=="rebuild" goto rebuild
goto build

:configure
echo === Configuring for Nintendo Switch ===
mkdir "%BUILD_DIR%" 2>nul
cd /d "%BUILD_DIR%"
cmake .. -DCMAKE_TOOLCHAIN_FILE=..\toolchains\switch.cmake -DCMAKE_BUILD_TYPE=Release -G Ninja
echo === Configure done ===
exit /b

:build
if not exist "%BUILD_DIR%\build.ninja" call :configure
echo === Building ===
cd /d "%BUILD_DIR%"
ninja
if %errorlevel% neq 0 exit /b %errorlevel%
echo === NACP ===
nacptool --create "Silent Hill" "ChaykaDed" "1.0" SilentHillPC.nacp
echo === NRO ===
elf2nro SilentHillPC SilentHillPC.nro --nacp=SilentHillPC.nacp
echo === Icon ===
python3 ..\maker.py --nro SilentHillPC.nro --icon ..\SH1icon.png --author "ChaykaDed"
echo === Done: %BUILD_DIR%\SilentHillPC.nro ===
exit /b

:clean
echo === Cleaning ===
rmdir /s /q "%BUILD_DIR%" 2>nul
echo === Cleaned ===
exit /b

:rebuild
call :clean
call :build
exit /b
