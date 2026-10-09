@echo off
chcp 936 >nul
title 板子数据 → 网页（关掉本窗口即停止）
echo.
echo   ==================================================
echo    正在启动： 服务端 + 串口桥接 + 浏览器
echo.
echo    1) 板子插好，串口号默认 COM4
echo    2) 浏览器会自动打开 http://127.0.0.1:8080
echo    3) 页面每 1.5 秒自动刷新，显示板子的真实读数
echo.
echo    停止： 关掉本窗口，或在窗口里按 Ctrl+C
echo   ==================================================
echo.
"C:\Users\user\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "%~dp0tools\serial_bridge.py" -p COM4 --with-server --open
echo.
echo   已停止。按任意键关闭本窗口。
pause >nul
