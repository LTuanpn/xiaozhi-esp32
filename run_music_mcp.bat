@echo off
title Xiaozhi Robot Music MCP Server
color 0B
cd /d "%~dp0"

echo [INFO] Dang khoi dong Xiaozhi Music MCP Server bang Python 3.13...
echo.

py -3.13 mcp_music_server\server.py
if errorlevel 1 (
    echo.
    echo [ERROR] Server da ket thuc voi ma loi %ERRORLEVEL%.
    pause
)
