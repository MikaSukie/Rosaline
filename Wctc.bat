@echo off
cls

make -f WINMakeFile clean
if errorlevel 1 exit /b %errorlevel%

powershell -Command "Measure-Command { make -f WINMakeFile }"