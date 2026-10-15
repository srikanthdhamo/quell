@echo off
rem Build Quell on Windows -> VST3 + standalone, packaged into dist\.
rem Requires: Visual Studio 2022/2026 (or Build Tools) with the C++ workload. Git for the JUCE download.
rem Builds in C:\qb because the Windows resource compiler fails on long build paths.
setlocal
cd /d "%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo Visual Studio C++ tools not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

rem Long paths for the JUCE download, for this script only (doesn't touch your git config)
set "GIT_CONFIG_COUNT=1"
set "GIT_CONFIG_KEY_0=core.longpaths"
set "GIT_CONFIG_VALUE_0=true"
if not defined QUELL_BUILD set "QUELL_BUILD=C:\qb"

cmake -S . -B "%QUELL_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%QUELL_BUILD%" || exit /b 1
"%QUELL_BUILD%\QuellEngineTest_artefacts\Release\QuellEngineTest.exe" || exit /b 1

for /f "tokens=3" %%v in ('findstr /r /c:"project(Quell VERSION" CMakeLists.txt') do set "VERSION=%%v"
set "PKG=dist\Quell-%VERSION%-Windows-x64"
if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%"
xcopy /e /i /q "%QUELL_BUILD%\Quell_artefacts\Release\VST3\Quell.vst3" "%PKG%\Quell.vst3" >nul
copy /y "%QUELL_BUILD%\Quell_artefacts\Release\Standalone\Quell.exe" "%PKG%\Quell-Standalone.exe" >nul
copy /y packaging\INSTALL-Windows.txt "%PKG%\INSTALL.txt" >nul
powershell -NoProfile -Command "Compress-Archive -Force -Path '%PKG%' -DestinationPath '%PKG%.zip'"
echo Packaged %PKG%.zip
