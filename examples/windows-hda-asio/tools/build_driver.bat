@echo off
REM build_driver.bat -- Build the kernel bridge driver using MinGW (w64devkit)
REM
REM No WDK needed! w64devkit has DDK headers, ntoskrnl.lib, and KS GUIDs.

setlocal

set GCC=C:\Users\elija\w64devkit\bin\gcc.exe
set SRC=..\driver\hda_bridge.c
set OUT=hda_bridge.sys
set DDK_INC=C:\Users\elija\w64devkit\include\ddk

echo === Building HDA ASIO Bridge Kernel Driver ===
echo.

if not exist "%GCC%" (
    echo ERROR: gcc not found at %GCC%
    echo Install w64devkit or update the path.
    exit /b 1
)

echo Compiling %SRC%...
%GCC% -nostdlib -nostartfiles -shared ^
    -Wall -Wno-unknown-pragmas ^
    -o %OUT% %SRC% ^
    -I%DDK_INC% ^
    -Wl,--subsystem,native ^
    -Wl,--entry,DriverEntry ^
    -lntoskrnl -lhal -lks -lksguid

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD SUCCEEDED: %OUT%
echo.
echo Next steps:
echo   1. Enable test signing: bcdedit /set testsigning on (requires reboot)
echo   2. Install: install.ps1 (as admin)
echo   3. Or manually: sc create HdaAsioBridge type= kernel binPath= "%CD%\%OUT%"

endlocal
