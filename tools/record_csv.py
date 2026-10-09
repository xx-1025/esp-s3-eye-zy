#!/usr/bin/env python3
"""
把板子串口打印的 [DATA] 行录成 CSV —— 用于交作业（"一条真实观测对应记录"）。

用法：
    录音 30 秒，自动保存到 samples/：
        python tools/record_csv.py -p COM4 -s 30

    实时监控并同时录音（Ctrl+C 结束，落盘已录部分）：
        python tools/record_csv.py -p COM4 --watch

输出：samples/board_<时间戳>.csv，列：序号, x, y, z, |a|, roll, pitch, 状态, 电脑接收时刻
"""
import argparse
import csv
import datetime as dt
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial，请先执行：pip install pyserial')

# 匹配形如：
# [DATA] #14  x=+0.141  y=+0.305  z=-0.950 g   |a|=1.0075 g   roll=+162.2  pitch=  -8.0 deg   静止 (...)
LINE_RE = re.compile(
    r'\[DATA\]\s*#(\d+)\s+'
    r'x=([+-]?[\d.]+)\s+y=([+-]?[\d.]+)\s+z=([+-]?[\d.]+)\s*g\s+'
    r'\|a\|=([\d.]+)\s*g\s+'
    r'roll=([+-]?[\d.]+)\s+pitch=\s*([+-]?[\d.]+)\s*deg\s*(.*)'
)


def main():
    ap = argparse.ArgumentParser(description='把串口 [DATA] 行录成 CSV')
    ap.add_argument('-p', '--port', default='COM4', help='串口号，默认 COM4')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='波特率，默认 115200')
    ap.add_argument('-s', '--seconds', type=float, default=30,
                    help='录音秒数，默认 30（用 --watch 时忽略）')
    ap.add_argument('-w', '--watch', action='store_true',
                    help='一直录到按 Ctrl+C')
    ap.add_argument('-o', '--out', default='samples', help='输出目录，默认 samples/')
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:                                  # noqa: BLE001
        sys.exit(f'打不开 {args.port}：{exc}')

    os.makedirs(args.out, exist_ok=True)
    stamp = dt.datetime.now().strftime('%Y%m%d_%H%M%S')
    path = os.path.join(args.out, f'board_{stamp}.csv')

    if args.watch:
        print(f'# 录制中（Ctrl+C 结束）→ {path}\n')
    else:
        print(f'# 录制 {args.seconds:g} 秒 → {path}\n')

    deadline = None if args.watch else time.time() + args.seconds
    buf = b''
    rows = []

    try:
        with open(path, 'w', newline='', encoding='utf-8-sig') as f:
            w = csv.writer(f)
            w.writerow(['seq', 'x_g', 'y_g', 'z_g', 'mag_g', 'roll_deg', 'pitch_deg', 'state', 'pc_time'])
            while deadline is None or time.time() < deadline:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                buf += chunk
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    line = raw.decode('utf-8', errors='replace').rstrip('\r')
                    m = LINE_RE.search(line)
                    if not m:
                        continue
                    seq, x, y, z, mag, roll, pitch, state = m.groups()
                    row = [seq, x, y, z, mag, roll, pitch, state.strip(),
                           dt.datetime.now().strftime('%H:%M:%S')]
                    w.writerow(row)
                    rows.append(row)
                    f.flush()
                    print(f'  已录第 {seq} 条  |a|={mag} g')
    except KeyboardInterrupt:
        pass
    finally:
        # 只关串口，不要手动 setDTR/setRTS（ESP32-S3 原生 USB 会被打进下载模式）
        ser.close()

    print(f'\n# 完成：共 {len(rows)} 条 → {path}')
    if not rows:
        print('# ⚠️  一条都没录到：确认板子已烧离线固件、串口号正确、没被别的工具占用')


if __name__ == '__main__':
    main()
