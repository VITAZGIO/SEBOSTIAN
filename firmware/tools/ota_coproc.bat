@echo off
chcp 65001 >nul
rem Прошивка СО-ПРОЦЕССОРА по Wi-Fi.
rem Сначала на экране головы: СИСТЕМА -> «OTA СОПРОЦ.», ждать «ЖДУ <IP>» (лента фиолетовая). 5 минут на заливку.
set PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe
if not exist "%PY%" set PY=python
set /p HOST=IP со-процессора с экрана головы [sebastian-coproc.local]: 
if "%HOST%"=="" set HOST=sebastian-coproc.local
set /p PW=Пароль OTA: 
"%PY%" "%~dp0ota_upload.py" %HOST% "%~dp0..\bin\sebastian_coproc.bin" --password "%PW%"
pause
