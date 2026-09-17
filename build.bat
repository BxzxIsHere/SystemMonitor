@echo off
setlocal
rem Builds the solution without needing the Visual Studio IDE open.
rem   build.bat            -> Release ^| x64
rem   build.bat Debug      -> Debug   ^| x64

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist %MSBUILD% set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

%MSBUILD% "%~dp0SystemMonitor\SystemMonitor.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /v:minimal /nologo
echo.
echo Output: %~dp0SystemMonitor\x64\%CONFIG%\SystemMonitor.exe
