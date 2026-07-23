@echo off

call csb\script\internal\bootstrap.bat
if errorlevel 1 exit /b %errorlevel%
build\csb.exe build %*
