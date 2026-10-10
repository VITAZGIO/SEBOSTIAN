@echo off
chcp 65001 >nul
rem ПЕРВАЯ прошивка на ESP-IDF (или аварийная) — ПО КАБЕЛЮ. Стирает флеш целиком и пишет полный образ.
rem   голова: порт UART (COM4),  со-процессор: порт CH340.
rem Использует esptool из PlatformIO (он у тебя уже стоит).
set PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe
set ESPTOOL=%USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py
if not exist "%PY%" set PY=python
if not exist "%ESPTOOL%" set ESPTOOL=-m esptool
echo 1 = ГОЛОВА (ESP32-S3)    2 = СО-ПРОЦЕССОР (ESP32)
set /p WHO=Что шьём [1/2]: 
set /p PORT=COM-порт (например COM4): 
if "%WHO%"=="1" (set CHIP=esp32s3& set BIN=sebastian_head_full.bin) else (set CHIP=esp32& set BIN=sebastian_coproc_full.bin)
echo Стираю флеш %CHIP% на %PORT% ...
"%PY%" %ESPTOOL% --chip %CHIP% --port %PORT% --baud 460800 erase_flash || goto err
echo Пишу %BIN% ...
"%PY%" %ESPTOOL% --chip %CHIP% --port %PORT% --baud 460800 write_flash 0x0 "%~dp0..\bin\%BIN%" || goto err
echo ГОТОВО. Плата перезагрузится сама (или нажми RESET).
pause
exit /b 0
:err
echo ОШИБКА. Порт верный? Монитор порта в VS Code закрыт? Иногда нужно держать BOOT при подключении.
pause
exit /b 1
