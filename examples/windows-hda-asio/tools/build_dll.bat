@echo off
REM build_dll.bat -- Build the ASIO DLL using MinGW (w64devkit)

setlocal

set GCC=C:\Users\elija\w64devkit\bin\gcc.exe
set SRC=..\dll\hda_asio.c
set DEF=..\dll\hda_asio.def
set OUT=hda_asio.dll

echo === Building HDA Direct ASIO DLL ===
echo.

if not exist "%GCC%" (
    echo ERROR: gcc not found at %GCC%
    echo Install w64devkit or update the path.
    exit /b 1
)

%GCC% -shared -O2 -Wall -Wno-unknown-pragmas ^
    -I..\include ^
    -o %OUT% %SRC% %DEF% ^
    -lole32 -loleaut32 -lsetupapi -lksuser -lavrt -luuid

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo BUILD SUCCEEDED: %OUT%
echo.
echo Next: Register with regsvr32 %OUT%

endlocal
