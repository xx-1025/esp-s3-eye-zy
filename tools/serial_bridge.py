#!/usr/bin/env python3
"""
串口桥接：把板子从串口打印的 [DATA] 行，转成 HTTP 上报灌进服务端。

为什么需要它：
    板子现在跑的是「离线模式」（不连 WiFi）。但服务端 / 网页那套是完全现成的，
    只要有人把数据"送上去"就行 —— 这个脚本就是那个搬运工：
        板子 --串口--> 本脚本 --HTTP POST /api/ingest--> server.js --> 网页
    板子固件**一行都不用改**。等以后 WiFi 通了，把 ENABLE_UPLOAD 改成 1，
    板子就能自己直传，这个脚本就可以退休了。

用法：
    # 只桥接（服务端要自己先起）
    python tools/serial_bridge.py -p COM4

    # 一键：自动起服务端 + 打开浏览器 + 桥接（推荐）
    python tools/serial_bridge.py -p COM4 --with-server --open

    # 桥 30 秒就停（方便快速验证）
    python tools/serial_bridge.py -p COM4 --seconds 30

遇到"串口一个字都没有"：先跑一次
    python tools/board_reset.py -p COM4
它会用完整复位（watchdog reset）把板子从下载模式拉回"跑固件"状态。

本脚本对串口掉线是容错的：板子复位/USB 重新枚举时不会崩，
会等设备回来再自动接上继续搬数据。

依赖：pyserial（pip install pyserial）
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import webbrowser

try:
    import serial
except ImportError:
    sys.exit('缺少 pyserial，请先执行：pip install pyserial')

# [DATA] #14  x=+0.140  y=+0.303  z=-0.940 g   |a|=0.9975 g   roll=+162.2  pitch=  -8.1 deg   静止 (...)
DATA_RE = re.compile(
    r'\[DATA\]\s*#(\d+)\s+'
    r'x=([+-]?[\d.]+)\s+y=([+-]?[\d.]+)\s+z=([+-]?[\d.]+)\s*g\s+'
    r'\|a\|=([\d.]+)\s*g\s+'
    r'roll=([+-]?[\d.]+)\s+pitch=\s*([+-]?[\d.]+)\s*deg\s*(.*)'
)
BOOT_RE = re.compile(r'\[BOOT\s*\]\s*fw=(\S+).*?dev=(\S+)')
IMU_RE = re.compile(r'命中\s+([A-Za-z0-9_]+)')

NODE_CANDIDATES = [
    os.environ.get('NODE_EXE', ''),
    r'C:\Users\user\.workbuddy\binaries\node\versions\22.22.2-2\node.exe',
    shutil.which('node') or '',
]

# 绕开系统代理：本地回环请求不该经过代理（否则沙箱/公司代理会把 127.0.0.1 拦掉）
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def release_port(ser):
    """
    只关串口 —— 千万不要在这里手动 setDTR/setRTS。

    ESP32-S3 走的是原生 USB（USB-Serial/JTAG），DTR/RTS 是**低有效**信号且直连
    GPIO0(BOOT) 和 EN：随便动一下就可能把板子顶进**下载模式**（表现：串口只有一个
    `boot:0x22 (DOWNLOAD...)`，之后一个字都不输出，看着像死机）。
    关闭时什么都不做最安全。恢复办法见 tools/board_reset.py。
    """
    if ser is None:
        return
    try:
        ser.close()
    except Exception:                                          # noqa: BLE001
        pass


def open_serial(port, baud):
    """打开串口 —— 打开前就把 DTR/RTS 设成低电平（GPIO0 高），避免顶进下载模式"""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def find_node():
    for c in NODE_CANDIDATES:
        if c and os.path.isfile(c):
            return c
    return None


def http_json(url, payload=None, token=None, timeout=3):
    headers = {}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    if token:
        headers['X-Device-Token'] = token
    req = urllib.request.Request(url, data=data, headers=headers, method='POST' if data else 'GET')
    with OPENER.open(req, timeout=timeout) as r:
        return r.status, json.loads(r.read().decode('utf-8'))


def guess_fw():
    """从 firmware/include/config.h 里读 FW_VERSION，当默认值用。

    为什么要这么做：板子重启后那行 `[BOOT] fw=... dev=...` 是开机瞬间打印的，
    桥接脚本开串口时通常已经错过了（USB CDC 在没人监听时会把数据丢掉），
    所以干脆从源码里取版本号——它本来就是编译期常量，不会错。
    """
    here = os.path.dirname(os.path.abspath(__file__))
    cfg = os.path.join(os.path.dirname(here), 'firmware', 'include', 'config.h')
    try:
        with open(cfg, encoding='utf-8') as fh:
            for line in fh:
                m = re.match(r'\s*#define\s+FW_VERSION\s+"([^"]+)"', line)
                if m:
                    return m.group(1)
    except Exception:                                          # noqa: BLE001
        pass
    return 'unknown'


def server_alive(base):
    try:
        http_json(base.rstrip('/') + '/api/health', timeout=1.5)
        return True
    except Exception:                                          # noqa: BLE001
        return False


def start_server(args):
    """起 server.js，返回进程对象（复用已有服务时返回 None）"""
    base = args.server.rstrip('/')
    if server_alive(base):
        print(f'# 检测到 {base} 已有服务在跑，直接复用\n')
        return None

    node = find_node()
    if not node:
        sys.exit('找不到 node.exe：请加 --node <路径> 或设环境变量 NODE_EXE')

    here = os.path.dirname(os.path.abspath(__file__))
    server_dir = os.path.join(os.path.dirname(here), 'server')
    port = base.rsplit(':', 1)[-1].split('/')[0]
    host = base.split('//', 1)[-1].split(':')[0]

    env = os.environ.copy()
    env['DEVICE_TOKEN'] = args.token
    env['HOST'] = host
    env['PORT'] = port

    print(f'# 启动服务端：{node} server.js  (HOST={host} PORT={port})')
    proc = subprocess.Popen([node, 'server.js'], cwd=server_dir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    for _ in range(40):                                        # 最多等 4 秒
        if proc.poll() is not None:
            sys.exit('服务端启动失败（进程已退出），检查 node 是否可用')
        if server_alive(base):
            print('# 服务端已就绪\n')
            return proc
        time.sleep(0.1)
    proc.terminate()
    sys.exit('服务端启动超时')


def main():
    ap = argparse.ArgumentParser(description='串口 → HTTP 桥接')
    ap.add_argument('-p', '--port', default='COM4', help='串口号，默认 COM4')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='波特率，默认 115200')
    ap.add_argument('--server', default='http://127.0.0.1:8080', help='服务端地址')
    ap.add_argument('--token', default='dev-token-please-change', help='必须与服务端 DEVICE_TOKEN 一致')
    ap.add_argument('--device', default='g01-s3eye',
                    help='设备 ID 兜底值（若抓到板子开机日志里的 dev= 会优先用那个）')
    ap.add_argument('--sensor', default='QMA6100P',
                    help='传感器型号（写进记录供页面显示，默认按本组板子填）')
    ap.add_argument('--fw', default='',
                    help='固件版本（留空则自动从 firmware/include/config.h 里读）')
    ap.add_argument('-s', '--seconds', type=float, default=0, help='桥接秒数，0=一直跑（默认）')
    ap.add_argument('--with-server', action='store_true', help='顺带把服务端也起起来')
    ap.add_argument('--open', action='store_true', help='顺带打开浏览器')
    args = ap.parse_args()

    srv_proc = start_server(args) if args.with_server else None
    if args.open:
        webbrowser.open(args.server)

    ser = None

    device_id = args.device
    fw = args.fw or guess_fw()
    sensor = args.sensor
    sent = 0
    failed = 0
    deadline = None if args.seconds <= 0 else time.time() + args.seconds
    t0 = time.time()

    # 注意：这里**不做任何 DTR/RTS 复位操作**。
    # 本板是 ESP32-S3 原生 USB，手工玩控制线会把板子打进下载模式。
    # 想拿板子的 device_id / fw，先跑 read_serial.py --watch，再按一下板子上的 RST 键。
    print(f'# 桥接中：{args.port} → {args.server}/api/ingest')
    print(f'# device_id={device_id}  sensor={sensor}  fw={fw}')
    print(f'# 板子不用改任何配置；按 Ctrl+C 结束\n')

    buf = b''
    try:
        while deadline is None or time.time() < deadline:
            # 串口还没拿到（刚开机 / 板子正在复位）就等一下再用
            if ser is None:
                try:
                    ser = open_serial(args.port, args.baud)
                    print(f'# 已连接 {args.port}，开始搬运数据')
                except Exception as exc:                       # noqa: BLE001
                    print(f'# 等待串口 {args.port} 可用 …（{exc}）')
                    time.sleep(1.0)
                    continue

            try:
                chunk = ser.read(4096)
            except Exception as exc:                           # noqa: BLE001
                # ESP32-S3 原生 USB 一旦复位，当前句柄立刻失效，
                # Windows 会抛 ClearCommError/PermissionError。
                # 这里不退出，丢掉旧句柄等设备重新枚举后自动接上。
                print(f'# 串口中断（{exc}），等待设备重新接入 …')
                release_port(ser)
                ser = None
                time.sleep(1.0)
                continue

            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                line = raw.decode('utf-8', errors='replace').rstrip('\r')

                m = BOOT_RE.search(line)
                if m:
                    fw = m.group(1)
                    if not device_id:
                        device_id = m.group(2)
                    print(f'# 板子：fw={fw}  device_id={device_id}')
                    continue

                m = IMU_RE.search(line)
                if m and '命中' in line:
                    sensor = m.group(1)
                    print(f'# 传感器：{sensor}')
                    continue

                m = DATA_RE.search(line)
                if not m:
                    continue

                seq, x, y, z, mag, roll, pitch, state = m.groups()
                if not device_id:
                    device_id = 'serial-unknown'

                payload = {
                    'device_id': device_id,
                    'seq': int(seq),
                    'metric': 'accel_mag',
                    'value': float(mag),
                    'unit': 'g',
                    'axes': {
                        'accel': {'x': float(x), 'y': float(y), 'z': float(z), 'unit': 'g'},
                        'gyro': None,
                    },
                    'sensor': sensor,
                    'has_gyro': False,
                    'roll_deg': float(roll),
                    'pitch_deg': float(pitch),
                    'state': state.strip(),
                    # 板端离线模式没有 NTP，时间用"桥接端收到的那一刻"，
                    # 并显式标注来源，避免和真正的板端时间混淆
                    'ts_device': int(time.time()),
                    'ts_source': 'bridge_pc',
                    'uptime_ms': int((time.time() - t0) * 1000),
                    'rssi': 0,
                    'mac': '',
                    'fw': fw,
                    'sampling': True,
                    'via': 'serial-bridge',
                }

                try:
                    code, resp = http_json(args.server.rstrip('/') + '/api/ingest',
                                           payload, token=args.token)
                    if code == 201:
                        sent += 1
                        print(f'  #{seq}  |a|={mag} g   → HTTP 201 (累计 {sent} 条)')
                    else:
                        failed += 1
                        print(f'  #{seq}  上报被拒：HTTP {code} {resp.get("error", "")}')
                except Exception as exc:                       # noqa: BLE001
                    failed += 1
                    print(f'  #{seq}  上报失败：{exc}')
    except KeyboardInterrupt:
        pass
    finally:
        release_port(ser)
        if srv_proc:
            print('# 停止服务端')
            srv_proc.terminate()

    print(f'\n# 桥接结束：成功 {sent} 条，失败 {failed} 条')
    if sent:
        print(f'# 看板：{args.server}/')


if __name__ == '__main__':
    main()
