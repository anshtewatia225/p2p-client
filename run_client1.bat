@echo off
REM Start a P2P Client
set PATH=C:\msys64\ucrt64\bin;%PATH%
echo Starting P2P Client on port 6000...
client.exe 127.0.0.1:6000 tracker_info.txt
pause
