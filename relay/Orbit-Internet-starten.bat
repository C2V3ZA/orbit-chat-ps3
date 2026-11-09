@echo off
title Orbit Internet - Fenster offen lassen
cd /d "%~dp0"
echo.
echo   Orbit Internet fuer PS3
echo   -----------------------
echo   Ein Fenster, ein Programm: Vermittler + Internet-Tunnel (bore.pub).
echo   Kein bore.exe noetig. Dieses Fenster offen lassen, solange ihr Orbit nutzt.
echo   Fragt Windows nach der Firewall: "Zulassen" fuer private Netzwerke klicken.
echo.
where python >nul 2>nul
if not errorlevel 1 goto run
echo   Python wurde nicht gefunden. Bitte von https://www.python.org/downloads/ installieren
echo   und dabei "Add python.exe to PATH" anhaken.
goto end
:run
python orbit_relay.py --host 0.0.0.0 --port 31400 --bore bore.pub --bore-port 31400 --state "%~dp0owners.json" --log "%~dp0orbit-relay.log"
:end
echo.
echo   Orbit Internet wurde beendet.
pause
