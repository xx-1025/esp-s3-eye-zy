#!/usr/bin/env python3
"""
读取开发板串口日志。

两种用法：
    1) 采集固定秒数后自动退出（默认，不用手动按 Ctrl+]）：
           python tools/read_serial.py -p COM4 -s 20
    2) 实时监控，一直打印到按 Ctrl+C（推荐看板子跑成什么样）：
           python tools/read_serial.py -p COM4 --watch

想抓开机那几行（[BOOT]/[CAL]/[IMU]）：先跑起本脚本，再按一下板子上的 RST 键即可。
本脚本**不做任何软复位** —— ESP32-S3 是原生 USB，手工玩 DTR/RTS 会把板子打进下载模式。

依赖：pyserial（pip install pyserial）
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial，请先执行：pip install pyserial')


def main():
    ap = argparse.ArgumentParser(description='读串口日志')
    ap.add_argument('-p', '--port', default='COM4', help='串口号，如 COM4 / /dev/ttyUSB0')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='波特率，默认 115200')
    ap.add_argument('-s', '--seconds', type=float, default=20,
                    help='采集秒数，默认 20（用 --watch 时忽略）')
    ap.add_argument('-w', '--watch', action='store_true',
                    help='实时监控模式：一直打印，直到按 Ctrl+C 退出')
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:                                  # noqa: BLE001
        sys.exit(f'打不开 {args.port}：{exc}\n'
                 f'提示：确认串口号（Windows 看设备管理器），并关掉其他占用它的工具。')

    # 想抓开机那几行 [BOOT]/[CAL]？很简单：先跑起本脚本，再按一下板子上的 RST 键。
    # ⚠ 千万别在代码里手工 setDTR/setRTS 做"软复位"：
    #   ESP32-S3 用的是原生 USB（USB-Serial/JTAG），DTR/RTS 在芯片内部直连 EN 和 GPIO0，
    #   USB-UART 板子那套时序（DTR 拉低再拉高）会把 S3 顶进**下载模式**，
    #   之后串口一个字都不输出，只能重烧固件或拔插 USB 才能恢复。
    if args.watch:
        print(f'# 实时监控 {args.port} @ {args.baud} —— 按 Ctrl+C 退出\n')
    else:
        print(f'# 已打开 {args.port} @ {args.baud}，采集 {args.seconds:g} 秒 ...\n')

    deadline = None if args.watch else time.time() + args.seconds
    buf = b''
    try:
        while deadline is None or time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                print(line.decode('utf-8', errors='replace').rstrip('\r'), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        # 只关串口，不要手动 setDTR/setRTS：
        # ESP32-S3 原生 USB 的这两个信号直连 EN/GPIO0，乱动会把板子打进下载模式
        ser.close()
    print('\n# 已退出监控' if args.watch else '\n# 采集结束')


if __name__ == '__main__':
    main()
