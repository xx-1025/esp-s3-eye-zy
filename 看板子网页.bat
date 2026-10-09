@echo off
chcp 936 >nul
title 看板子网页（关掉本窗口 = 停止搬运）
echo.
echo   ==============================================================
echo     ESP32-S3-EYE 传感器数据  =^>  网页看板
echo.
echo     1) 自动把板子恢复成"正在跑固件"的状态
echo     2) 启动服务端 + 打开浏览器 + 把串口数据搬上网页
echo.
echo     看完直接关掉本窗口即可（浏览器可以留着）
echo   ==============================================================
echo.

set "PY=C:\Users\user\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
set "ROOT=%~dp0"

echo   [1/3] 检查板子状态，必要时做一次完整复位 ...
"%PY%" "%ROOT%tools\board_reset.py" -p COM4 --quiet
echo.

echo   [2/3] 启动服务端 + 打开网页（第一次打开可能等两三秒）...
echo   [3/3] 开始搬运数据（看到 "# ... -^> HTTP 201" 就是在正常上报）
echo.
"%PY%" -u "%ROOT%tools\serial_bridge.py" -p COM4 --with-server --open

echo.
echo   已停止搬运。按任意键关闭本窗口。
pause >nul
