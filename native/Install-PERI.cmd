@echo off
setlocal
tasklist /fi "imagename eq obs64.exe" | find /i "obs64.exe" >nul
if not errorlevel 1 (
  echo Close OBS Studio before installing PERI.
  pause
  exit /b 1
)
set "dest=%ProgramData%\obs-studio\plugins\peri-output"
if not exist "%dest%\bin\64bit" mkdir "%dest%\bin\64bit"
if not exist "%dest%\data" mkdir "%dest%\data"
copy /y "%~dp0peri-output\bin\64bit\peri_obs.dll" "%dest%\bin\64bit\peri_obs.dll" >nul
if errorlevel 1 exit /b 1
copy /y "%~dp0peri-output\bin\64bit\peri-output.dll" "%dest%\bin\64bit\peri-output.dll" >nul
if errorlevel 1 (
  echo Install failed. Extract the ZIP first and run this installer as administrator.
  pause
  exit /b 1
)
echo Installed. Open OBS - Settings - Stream - PERI.
echo Enter peri://IP:port in Server, then paste your stream key.
pause
