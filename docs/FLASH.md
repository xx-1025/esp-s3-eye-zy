# 烧录手册 —— 从零到板子跑起来

> 面向第一次用 PlatformIO 的人。照着从上往下做，10~20 分钟。
> 全程只需要一个命令行 + 一根 USB 线，不用装 GUI 软件（除非你更想要图形界面，见路线 B）。

---

## 第 0 步：先确定你手上是哪块板

这一步决定后面用哪条编译目标、要不要装驱动。

| 你的板子 | 特征 | PlatformIO 目标 | USB 驱动 |
|---|---|---|---|
| **ESP32-S3-EYE** | 方板，有摄像头 + LCD，**板上只有一个 USB-C** | `esp32s3eye` | **不用装**（原生 USB） |
| **ESP-EYE**（老款） | 长条板，摄像头 + 麦克风，**有一个 micro-USB** | `esp-eye` | CP2102，Win10/11 一般自动装 |
| 通用 ESP32 开发板 | DevKit 长条 | `esp-eye` | CH340/CP2102 |

**怎么看**：插上 USB 后，Windows「设备管理器 → 端口(COM 和 LPT)」里出现的名字：

- 出现 **`USB 串行设备 (COMx)`** 或 `USB Serial Device` → 多半是 ESP32-S3-EYE（原生 USB）
- 出现 **`Silicon Labs CP210x (COMx)`** → ESP-EYE 或带 CP2102 的板子
- 出现 **`USB-SERIAL CH340 (COMx)`** → 国产 DevKit

> 记住这个 `COMx`，后面 `-t upload` 靠 PlatformIO 自动找，找不到时才需要手动指定。

---

## 第 1 步：装 PlatformIO（选一条）

### 路线 A：命令行版（推荐，最干净）

PlatformIO 是个 Python 包，装起来很简单。

**Windows（PowerShell 或 CMD）**：

```powershell
# 1) 装 Python（如果还没有）：微软商店搜 "Python 3.12" 安装，或去 python.org 下
#    勾选 "Add python.exe to PATH"

# 2) 装 PlatformIO
pip install -U platformio

# 3) 验证（应打印一个版本号，如 6.1.x）
pio --version
```

如果 `pip` 提示找不到命令，用：

```powershell
python -m pip install -U platformio
python -m platformio --version
```

**macOS / Linux**：

```bash
python3 -m pip install -U platformio
~/.local/bin/pio --version      # 或直接 pio --version
```

### 路线 B：VS Code 插件（图形界面，点点点）

1. 装 [VS Code](https://code.visualstudio.com/)
2. 扩展市场搜索 **PlatformIO IDE**（作者 PlatformIO），安装
3. 装完左下角会出现一个蚂蚁头图标 → 点它打开 PlatformIO 主页
4. 用 VS Code「打开文件夹」打开本项目的 `esp32-sensor-web/firmware` 目录
5. 左侧 PlatformIO 面板里选 `esp32s3eye` 环境 → 点 **Upload**

---

## 第 2 步：改配置（**必做，不改必失败**）

打开 `firmware/include/config.h`，改这 4 处：

```c
#define WIFI_SSID       "你的2.4G WiFi名"      // ESP32 不支持 5G！
#define WIFI_PASSWORD   "你的WiFi密码"
#define SERVER_INGEST_URL  "http://192.168.1.100:8080/api/ingest"
#define DEVICE_ID          "g01-s3eye"          // 全班唯一
```

- `SERVER_INGEST_URL` 的 IP：如果你先在本机跑服务端，就在电脑上执行 `ipconfig`（Windows）
  或 `ip addr`（Linux/mac）找到 **局域网 IPv4**（一般是 `192.168.x.x` 或 `10.x.x.x`），
  **不要用 `127.0.0.1`**（板子连不到你电脑的 localhost）。
- `DEVICE_TOKEN` 保持默认 `dev-token-please-change` 也行，但**必须和服务端启动时的环境变量一致**。

> `config.h` 是 gitignore 的（含 WiFi 密码）。仓库里那份叫 `config.example.h`。
> 如果你手上没有 `config.h`，先复制一份：
> ```bash
> cp firmware/include/config.example.h firmware/include/config.h
> ```

---

## 第 3 步：接线 + 进下载模式

1. USB 线插到板子的 **USB 口**，另一头插电脑
2. **ESP32-S3-EYE 特别注意**：这块板**没有 USB 转串口芯片**，用原生 USB。
   如果 `-t upload` 报 `Failed to connect`，按下面手动进下载模式：
   - 按住板上的 **BOOT** 键不放
   - 按一下 **RST**（或拔插 USB）
   - 松开 **BOOT**
   - 再执行 upload 命令
3. **ESP-EYE / 通用 DevKit**：一般插上就能烧，不用按键。

---

## 第 4 步：编译 + 烧录

```bash
cd esp32-sensor-web/firmware

# ESP32-S3-EYE
pio run -e esp32s3eye -t upload

# ESP-EYE / 通用 ESP32
pio run -e esp-eye -t upload
```

**第一次会慢**（PlatformIO 要下载编译器工具链和 ESP32 框架，几百 MB），后面就快了。
看到类似下面的输出就是成功：

```
Building in release mode
Compiling .pio/build/esp32s3eye/src/main.cpp.o
...
Linking .pio/build/esp32s3eye/firmware.elf
...
Writing at 0x00010000... (100%)
Leaving...
Hard resetting via RTS pin...
========================= [SUCCESS] Took 42.13 seconds =========================
```

> 只想编译不烧录：把 `-t upload` 去掉 → `pio run -e esp32s3eye`
> 想先清空板子（换过配置导致异常时有用）：`pio run -e esp32s3eye -t erase`

---

## 第 5 步：看串口日志（**验证的关键**）

```bash
pio device monitor -e esp32s3eye -b 115200
```

（VS Code 插件：点左下角「插头」图标 → Serial Monitor）

正常应该看到：

```
[BOOT ] fw=week1-1.2 board=ESP32-S3-EYE dev=g01-s3eye
[I2C  ] 扫描到 1 个设备: 0x12
[IMU  ] 命中 QMA7981 @0x12（只有三轴加速度，无陀螺仪）  ±2g / 4096 LSB/g
[WIFI ] 已连接  ip=192.168.1.30  rssi=-52 dBm
[TIME ] NTP 对准，板端 UTC = 1760000000
[READY] 开始采样，服务端 = http://192.168.1.100:8080/api/ingest
[DATA ] #1 accel=(0.011,-0.022,0.999)g mag=1.0032g  gyro=n/a  -> HTTP 201 (ok=1)
```

**退出 monitor**：按 `Ctrl + ]`（不是 Ctrl+C）。

> 💡 更省事：`python ../tools/read_serial.py -p COM4 -s 16`
> 它会先给板子发硬复位信号，然后抓 16 秒日志自动退出，能把从 `[BOOT]` 开始的完整开机日志都抓到。

> 💡 **第一次跑，建议先用离线模式**：`config.h` 里 `ENABLE_UPLOAD = 0`，
> 不联网、不校时、不上报，串口直接出真实数值。跑通了再改成 `1` 上服务器。

### 三种日志分别说明什么

| 看到的 | 说明 | 下一步 |
|---|---|---|
| `[I2C] 扫描到 1 个设备: 0x12` | 只有板载加速度计 | 想要陀螺仪 → 外接 MPU6050（看 README 第 2 节） |
| `[I2C] 扫描到 2 个设备: 0x12 0x68` + `[IMU] 命中 MPU6050 ... 三轴加速度 + 三轴陀螺仪` | 六轴识别成功 ✅ | 直接进第 6 步验证 |
| `[I2C] 扫描到 0 个设备` / `[IMU] ❌ 没识别到 IMU` | 传感器没认出来 | 看下面 `[PROBE]` 探针表，贴给我 |

---

## 第 6 步：确认数据真的上传了

1. 服务端在跑（`cd server && node server.js`）
2. 浏览器打开 `http://<服务器IP>:8080/` → 应看到实时数值 + 曲线
3. 命令行核对：
   ```bash
   curl "http://<服务器IP>:8080/api/latest?device=g01-s3eye"
   ```
   返回的 `value` 应和页面上那个大数字**完全一致**。

---

## 常见烧录错误对照表

| 报错 | 原因 | 怎么修 |
|---|---|---|
| `Failed to connect to ESP32-S3: No serial data received` | 没进下载模式 / 端口被占 | 按住 BOOT 再插 USB；关掉其他串口软件（Arduino IDE、其他 monitor） |
| `could not open port 'COMx'` | 端口被占用 | 关掉所有串口工具；拔插 USB 换个口 |
| `Please specify `upload_port``| 有多个串口，PlatformIO 不知道用哪个 | 加参数：`pio run -e esp32s3eye -t upload --upload-port COM5` |
| 编译时 `Unknown board ID 'esp32-s3-devkitc-1'` | 平台没装好 | 先跑一次 `pio pkg install -e esp32s3eye`，或删掉 `~/.platformio` 重来 |
| `A fatal error occurred: Timed out waiting for packet header` | 板子在跑程序没复位 | 按住 BOOT 后按 RST 松开；或 `pio run -t upload` 前先断电重插 |
| 烧完串口一片乱码 | 波特率不对 | 必须 `-b 115200` |
| 烧完不停重启 | 供电不足 / 配置不符 | 换一根**数据线**（不是充电线）；确认 `platformio.ini` 里 `qio_opi` 那行在 |
| Windows 认不到串口 | 驱动没装 | ESP-EYE/DevKit 装 [CP210x](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers) 或 [CH340](https://www.wch.cn/downloads/CH341SER_EXE.html) 驱动 |
| `pio: command not found` | PATH 没配 | 用 `python -m platformio ...` 代替 `pio ...` |

---

## 附：只想验证"能编译"（不插板子也行）

```bash
cd esp32-sensor-web/firmware
pio run -e esp32s3eye        # 只编译，不烧录
```

出现 `[SUCCESS]` 就说明代码没问题，剩下的就只是硬件和网络的事了。
