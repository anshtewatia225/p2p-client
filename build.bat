@echo off
REM Build script for P2P File Sharing System on Windows
REM Requires MSYS2 with MinGW-w64 UCRT64 installed

set PATH=C:\msys64\ucrt64\bin;%PATH%

echo Compiling tracker.exe...
g++ -std=c++11 -o tracker.exe tracker.cpp -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile tracker.exe
    exit /b 1
)
echo tracker.exe compiled successfully!

echo Compiling client.exe...
g++ -std=c++11 -o client.exe client.cpp -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile client.exe
    exit /b 1
)
echo client.exe compiled successfully!

echo.
echo Build complete! You can now run:
echo   tracker.exe tracker_info.txt 1
echo   client.exe 127.0.0.1:6000 tracker_info.txt
