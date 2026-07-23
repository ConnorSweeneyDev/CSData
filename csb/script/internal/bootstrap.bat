@echo off

if not exist "build" mkdir "build"

powershell -NoProfile -Command "if (-not (Test-Path 'build\csb.exe')) { exit 1 }; $exe = (Get-Item 'build\csb.exe').LastWriteTime; if ((Test-Path 'csb\csb.cpp') -and ((Get-Item 'csb\csb.cpp').LastWriteTime -gt $exe)) { exit 1 }; if ((Test-Path 'csb\csb.hpp') -and ((Get-Item 'csb\csb.hpp').LastWriteTime -gt $exe)) { exit 1 }; exit 0" >nul 2>&1
if errorlevel 1 (
  cl /nologo /EHsc /std:c++20 /O2 /Icsb\ /Fobuild\ /Febuild\csb.exe csb\csb.cpp
  if errorlevel 1 exit /b %errorlevel%
)
