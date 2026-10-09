@echo off
chcp 936 >nul
title ESP32-S3-EYE 实时监控（看完按 Ctrl+C 退出）
echo.
echo   ================================================
echo    正在打开串口 COM4，稍等一两秒...
echo    看完后按 Ctrl+C 退出监控，再按任意键关闭窗口
echo   ================================================
echo.
"C:\Users\user\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "%~dp0tools\read_serial.py" -p COM4 --watch
echo.
echo   已退出监控。按任意键关闭本窗口。
pause >nul
