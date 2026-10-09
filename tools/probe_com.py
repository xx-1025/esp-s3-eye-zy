#!/usr/bin/env python3
"""
COM 口探测工具：找出这颗 ESP32-S3 原生 USB 到底用什么电平组合才会输出数据。

背景：ESP32-S3 的 USB-Serial/JTAG 把 DTR/RTS 直接接到芯片的 GPIO0(BOOT) / EN，
      电平不对就会把芯片摁在复位或下载模式，串口一个字都不出。
      本脚本逐个组合试，并打印每种组合读到的字节数，用来定位问题。

用法：python tools/probe_com.py -p COM4
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial')


def try_combo(port, baud, dtr, rts, seconds=3.0):
    """用一种 DTR/RTS 组合打开并读 seconds 秒，返回 (字节数, 首包)"""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = dtr
    ser.rts = rts
    try:
        ser.open()
    except Exception as exc:                                   # noqa: BLE001
        return None, f'打开失败：{exc}'
    time.sleep(0.3)
    try:
        ser.reset_input_buffer()
    except Exception:                                          # noqa: BLE001
        pass
    n = 0
    first = None
    t = time.time()
    try:
        while time.time() - t < seconds:
            d = ser.read(4096)
            if d:
                if first is None:
                    first = d[:120]
                n += len(d)
    except Exception as exc:                                   # noqa: BLE001
        ser.close()
        return n, f'读取中断：{exc}'
    ser.close()
    return n, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default='COM4')
    ap.add_argument('-b', '--baud', type=int, default=115200)
    ap.add_argument('-s', '--seconds', type=float, default=3.0)
    args = ap.parse_args()

    combos = [
        ('DTR低 RTS低', False, False),
        ('DTR高 RTS低', True,  False),
        ('DTR低 RTS高', False, True),
        ('DTR高 RTS高', True,  True),
    ]
    for label, dtr, rts in combos:
        n, info = try_combo(args.port, args.baud, dtr, rts, args.seconds)
        if n is None:
            print(f'  {label}  -> {info}')
        else:
            print(f'  {label}  -> {n} 字节   {info!r}')


if __name__ == '__main__':
    main()
