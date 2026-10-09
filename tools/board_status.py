#!/usr/bin/env python3
"""
板子状态诊断：复位后打印完整 ROM 启动日志，用来判断芯片到底进了什么启动模式。

关键看点：日志里那一行
    rst:0x15 (USB_UART_CHIP_RESET),boot:0x?? (????)
它的括号里就是启动模式：
    SPI_FAST_FLASH_BOOT  -> 正常，应该跑我们的固件
    DOWNLOAD...          -> 进了下载模式，串口不会有任何应用输出（看着像死机）

用法：
    python tools/board_status.py -p COM4            # 默认 DTR=低（IO0 高 → 正常启动）
    python tools/board_status.py -p COM4 --dtr-high # 强制 DTR=高，作对照
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial')


def set_rts(ser, state):
    """Windows: 只改 RTS 不下发，必须同时写一次 DTR"""
    ser.setRTS(state)
    ser.setDTR(ser.dtr)


def capture(port, baud, dtr_at_reset, seconds=5.0, rounds=2):
    for r in range(rounds):
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
        except Exception as exc:                               # noqa: BLE001
            print(f'打开失败：{exc}')
            return
        label = 'DTR=高(IO0 低)' if dtr_at_reset else 'DTR=低(IO0 高)'
        print(f'\n===== 第 {r+1} 轮：复位期间 {label} =====')

        ser.setDTR(dtr_at_reset)
        set_rts(ser, True)          # EN = LOW，进入复位
        time.sleep(0.25)
        ser.setDTR(dtr_at_reset)    # 保持
        set_rts(ser, False)         # EN = HIGH，启动
        time.sleep(0.05)
        ser.setDTR(dtr_at_reset)    # ★ 启动后也保持不变，看它跑成什么

        t = time.time()
        buf = b''
        got = 0
        try:
            while time.time() - t < seconds:
                d = ser.read(4096)
                if not d:
                    continue
                got += len(d)
                buf += d
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    line = raw.decode('utf-8', errors='replace').rstrip('\r')
                    if line.strip():
                        print('   ', line)
        except Exception as exc:                               # noqa: BLE001
            print(f'    读取中断：{exc}')
        finally:
            try:
                ser.close()
            except Exception:                                  # noqa: BLE001
                pass
        print(f'    （本轮共 {got} 字节）')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default='COM4')
    ap.add_argument('-b', '--baud', type=int, default=115200)
    ap.add_argument('-s', '--seconds', type=float, default=5.0)
    ap.add_argument('--dtr-high', action='store_true', help='复位期间用 DTR=高（对照实验）')
    args = ap.parse_args()
    capture(args.port, args.baud, args.dtr_high, args.seconds)


if __name__ == '__main__':
    main()
