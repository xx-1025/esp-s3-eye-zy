#!/usr/bin/env python3
"""
让板子"真正重新启动"——把卡在下载模式的 ESP32-S3 拉回正常运行。

为什么需要它（重要，别再踩）：
    ESP32-S3 的原生 USB（USB-Serial/JTAG）有两个坑：

    1) DTR/RTS 是**低有效**信号（True = 0V）：
           DTR -> GPIO0(BOOT)   DTR=True  == GPIO0 拉低 ==> 开机进下载模式
           RTS -> EN(复位)      RTS=True  == EN 拉低   ==> 芯片被摁住
       而 pyserial 打开串口时默认把 DTR/RTS 都置 True，
       所以任何"软件复位"都可能把芯片送进下载模式。

    2) 更要命的是：**USB-JTAG 发出的复位不一定能重新采样启动引脚**。
       芯片一旦进了下载模式，用 DTR/RTS 怎么复位都回不到"跑固件"的状态
       （表现：串口只打印一次 `boot:0x22 (DOWNLOAD(USB/UART0))` / `waiting for download`，
        之后一个字都没有，看着像死机）。

    官方给的解法是"做一次完整复位"，而 esptool 正好有这个选项：
        --after watchdog_reset      (看门狗复位 = 完整复位，会重新采样 GPIO0)

本工具做的事：
    调用 PlatformIO 自带的 esptool，用 watchdog_reset 让芯片完整复位，
    然后打开串口读几秒，确认板子真的在跑固件（看到 [DATA] 就算成功）。

用法：
    python tools/board_reset.py -p COM4
    python tools/board_reset.py -p COM4 --quiet      # 只做事，少打印（给启动脚本用）
"""
import argparse
import glob
import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial，请先执行：pip install pyserial')


def find_esptool():
    """找 PlatformIO 自带的 esptool.py"""
    cands = [
        os.path.expanduser('~/.platformio/packages/tool-esptoolpy/esptool.py'),
        os.path.expanduser('~/.platformio/packages/tool-esptoolpy/esptool/esptool.py'),
    ]
    cands += glob.glob(os.path.expanduser('~/.platformio/packages/tool-esptoolpy*/*/esptool.py'))
    cands += glob.glob(os.path.expanduser('~/.platformio/packages/tool-esptoolpy*/esptool/*.py'))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def run_esptool(esptool, port, before):
    cmd = [sys.executable, esptool, '--port', port,
           '--before', before, '--after', 'watchdog_reset', 'flash_id']
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        return r.returncode == 0, (r.stdout or '') + (r.stderr or '')
    except subprocess.TimeoutExpired:
        return False, 'esptool 超时'


def peek(port, baud, seconds=6.0):
    """读几秒，返回 (字节数, 文本)"""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False          # GPIO0 保持高（正常启动）；且不要去动它，避免又进下载模式
    ser.rts = False
    try:
        ser.open()
    except Exception as exc:                                   # noqa: BLE001
        return 0, f'打不开串口：{exc}'
    t = time.time()
    buf = b''
    try:
        while time.time() - t < seconds:
            d = ser.read(4096)
            if d:
                buf += d
    except Exception as exc:                                   # noqa: BLE001
        return len(buf), f'读取中断：{exc}'
    finally:
        try:
            ser.close()
        except Exception:                                      # noqa: BLE001
            pass
    return len(buf), buf.decode('utf-8', errors='replace')


def main():
    ap = argparse.ArgumentParser(description='完整复位板子，让它跑固件')
    ap.add_argument('-p', '--port', default='COM4')
    ap.add_argument('-b', '--baud', type=int, default=115200)
    ap.add_argument('-s', '--seconds', type=float, default=6.0, help='复位后读几秒')
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()

    def log(*a):
        if not args.quiet:
            print(*a)

    esptool = find_esptool()
    if not esptool:
        sys.exit('找不到 esptool.py（应该在 ~/.platformio/packages/tool-esptoolpy/ 下）。\n'
                 '请先执行一次 pio run -t upload，或手动指定路径。')
    log(f'# esptool: {esptool}')

    # 先试 no_reset（板子已在下载模式时最省事），不行再用 default_reset
    for before in ('no_reset', 'default_reset'):
        ok, out = run_esptool(esptool, args.port, before)
        if ok:
            log(f'# 已用 --before {before} + watchdog_reset 完成完整复位')
            break
        log(f'# --before {before} 方式没成功，换一种…')
    else:
        log('# ⚠️ esptool 两次都没跑通，还是直接读读看')

    time.sleep(2.0)
    n, text = peek(args.port, args.baud, args.seconds)

    lines = [l for l in text.splitlines() if l.strip()]
    data_lines = [l for l in lines if '[DATA]' in l]
    if data_lines:
        log(f'# ✅ 板子正常运行中（读到 {n} 字节，含 {len(data_lines)} 条 [DATA]）')
        log(f'#    最近一条：{data_lines[-1].strip()}')
        return 0

    if 'DOWNLOAD' in text:
        log('# ❌ 还是下载模式。请按一下板子上的 RST 键，或拔插一次 USB，再重试。')
        log('#    （若板子上的 BOOT 键被按住/卡住，松开会好）')
        return 1

    log(f'# ⚠️ 读到 {n} 字节但没有 [DATA]。原始输出：')
    log(text[:500] if text.strip() else '  (空)')
    return 2


if __name__ == '__main__':
    sys.exit(main())
