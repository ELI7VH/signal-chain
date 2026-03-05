@echo off
REM build_test.bat -- Build the ASIO loopback test using MinGW (w64devkit)

setlocal

set GCC=C:\Users\elija\w64devkit\bin\gcc.exe
set SRC=..\test\asio_loopback.c
set OUT=asio_loopback.exe

echo === Building ASIO Loopback Test ===
echo.

%GCC% -O2 -Wall -Wno-unknown-pragmas ^
    -I..\include ^
    -o %OUT% %SRC% ^
    -lole32 -loleaut32

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo BUILD SUCCEEDED: %OUT%

endlocal
