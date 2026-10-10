@echo off
chcp 65001 >nul
rem Прошивка ГОЛОВЫ по Wi-Fi. Двойной щелчок — спросит адрес и пароль.
rem Нужен Python (есть у PlatformIO) — ищем сначала его, потом системный.
set PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe
if not exist "%PY%" set PY=python
set /p HOST=Адрес головы [sebastian-head.local]: 
if "%HOST%"=="" set HOST=sebastian-head.local
set /p PW=Пароль OTA: 
"%PY%" "%~dp0ota_upload.py" %HOST% "%~dp0..\bin\sebastian_head.bin" --password "%PW%"
pause
