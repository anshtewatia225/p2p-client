@echo off
REM Start the P2P Tracker Server
set PATH=C:\msys64\ucrt64\bin;%PATH%
echo Starting P2P Tracker Server...
tracker.exe tracker_info.txt 1
pause
