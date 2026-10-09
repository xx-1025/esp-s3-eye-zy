#pragma once

// =====================================================================
//  板端配置 —— 只改这个文件就够了
//  复制成 config.h 后按注释把 <尖括号> 里的内容替换掉
// =====================================================================

// ---------- 1. WiFi ----------
// 注意：ESP32-S3 只支持 2.4GHz，不要填 5G 的 SSID
#define WIFI_SSID       "<你的WiFi名称>"
#define WIFI_PASSWORD   "<你的WiFi密码>"

// ---------- 2. 服务器 ----------
// 本机调试（板子和电脑在同一个 WiFi 下）：
//   填电脑的局域网 IP，例如 "http://192.168.1.23:8080"
// 部署到 VPS 后：
//   填 "http://<VPS公网IP>:8080"  或  "http://<你的域名>"
#define SERVER_INGEST_URL  "http://192.168.1.100:8080/api/ingest"

// 必须和服务端环境变量 DEVICE_TOKEN 完全一致，否则会被 401 拒绝
#define DEVICE_TOKEN       "dev-token-please-change"

// ---------- 3. 本组设备标识（全班唯一！） ----------
// 命名建议：组号 + 板子，例如 "g03-s3eye"
#define DEVICE_ID          "g01-s3eye"

// ---------- 4. I2C 引脚 ----------
// ESP32-S3-EYE 板载 IMU（实测为 QMA6100P，ID=0x90）：SDA=GPIO4, SCL=GPIO5（默认不用改）
// 若外接带陀螺仪的 IMU（MPU6050/MPU9250/LSM6DS3/QMI8658...）：
//   挂在 S3-EYE 上 → 仍是 SDA=4, SCL=5（和板载共用一条总线）
//   挂在通用 ESP32 / ESP-EYE 上 → 改成 SDA=21, SCL=22
#define I2C_SDA_PIN        4
#define I2C_SCL_PIN        5
#define I2C_FREQ_HZ        400000

// ---------- 5. 采样与上报节奏 ----------
#define SAMPLE_INTERVAL_MS 1000   // 采样周期（毫秒）
#define UPLOAD_INTERVAL_MS 1000   // 上报周期（毫秒），>= 采样周期

// ---------- 6. NTP 校时 ----------
// 服务器拿不到时间会导致"板端时间"是 1970 年，页面时间对不上
#define NTP_SERVER_1       "ntp.aliyun.com"
#define NTP_SERVER_2       "pool.ntp.org"
#define NTP_TZ_OFFSET_SEC  (8 * 3600)   // 仅影响本地时间打印，上报用 UTC 秒

// ---------- 7. 其他 ----------
#define FW_VERSION         "week1-1.5"
#define BUTTON_PIN         0            // BOOT 键：短按 开始/暂停 采样；长按 ≥1.2s 重新静止校准

// ---------- 8. 运行模式 ----------
// 0 = 离线模式：只读传感器 + 串口打印。不连 WiFi、不校时、不上报、不依赖任何网络。
//      —— 建议先用这个把"真实读到数据"跑通（第 1 周的第一步）。
// 1 = 联网模式：连 WiFi → NTP 校时 → HTTP POST 上报
//      —— 确认上面 1/2 两项填对了再打开。
#define ENABLE_UPLOAD      0

// ---------- 9. 静止自动校准 ----------
// 1 = 开机后先让板子静止，采 100 个样本求均值，算出一个比例因子，
//     使静止时 |a| 精确落在 1.0000 g（三轴统一缩放，不改变读数方向）。
//     校准期间板子必须静止（放桌上别碰）；完成后会打印 [CAL] 结果。
//     换位置不用重校（比例与姿态无关）；读数明显漂了 → 长按 BOOT ≥1.2 秒重校。
// 0 = 关闭，直接用原始读数
#define ENABLE_AUTO_CALIB  1
