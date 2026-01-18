@echo off
REM Start a second P2P Client
set PATH=C:\msys64\ucrt64\bin;%PATH%
echo Starting P2P Client on port 6001...
client.exe 127.0.0.1:6001 tracker_info.txt
pause
