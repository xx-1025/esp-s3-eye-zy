/*
 * ============================================================================
 *  Week1 板端固件 —— 采集真实 IMU 数据（三轴加速度 + 三轴陀螺仪）→ WiFi 上传
 *
 *  设计要点（对应课堂 8 个环节）：
 *   1) 认识设备：开机扫描 I2C 总线，把"总线上到底挂了谁""每个地址的 WHO_AM_I
 *      是多少"全部打印出来 —— 这就是判断你手上到底有没有陀螺仪的唯一依据
 *   2) 分工：板端只做 "采样 + 打时间戳 + 上报"，不存历史、不做业务
 *   3) 真实传感源：自动识别，**优先选带陀螺仪的六轴芯片**，识别不到明确报警
 *       带陀螺仪（六轴）：
 *         - InvenSense 家族 @0x68/0x69  MPU6050 / MPU6000 / MPU6500 / MPU9250 /
 *                                          MPU9255 / MPU6886 / ICM-20602 /
 *                                          ICM-20608-G / ICM-20689
 *         - ST 家族      @0x6A/0x6B  LSM6DS3 / LSM6DSL / LSM6DS3TR-C / LSM6DSO /
 *                                    ISM330DLC / ASM330LHH / LSM6DSV
 *         - Bosch        @0x68/0x69  BMI160
 *         - QMI          @0x6A/0x6B  QMI8658
 *       只有加速度（无陀螺仪，页面会显示 n/a）：
 *         - QMA6100P     @0x12  ID 0x90   （QMA7981 的 pin-to-pin 兼容型号）
 *         - QMA7981      @0x12  ID 0xE7
 *         - ADXL345      @0x53            外接三轴加速度计
 *      识别不到就明确报警 + 打印探针表 + 每 5 秒重试，方便排查接线 —— 不做假数据兜底
 *   4) 核对单位与时基：上报带 unit( g / dps )、ts_device(UTC秒)、uptime_ms
 *   5) 停采演示：短按 BOOT(GPIO0) 暂停/恢复采样，页面会自己提示"未更新"；长按 ≥1.2s 重新静止校准
 *
 *  6) 两种运行模式（config.h 里的 ENABLE_UPLOAD）：
 *       =0 离线模式：只读传感器 + 串口打印，不联网、不校时、不上报 —— 先用这个跑通
 *       =1 联网模式：连 WiFi → NTP 校时 → HTTP POST 上报
 *  7) 静止自动校准（config.h 里的 ENABLE_AUTO_CALIB，默认开）：
 *       开机静止采 100 个样本求均值，把 |a| 归一到 1.0000 g，消除芯片零点/灵敏度误差；
 *       板子挪位置不用重校，读数漂了就长按 BOOT ≥1.2 秒重校。
 *
 *  串口输出样例（离线模式 + 自动校准，本组实测）：
 *      [BOOT ] fw=week1-1.5 board=ESP32-S3-EYE dev=g01-s3eye
 *      [I2C  ] 扫描到 1 个设备: 0x12
 *      [IMU  ] 命中 QMA6100P @0x12（只有三轴加速度，无陀螺仪）  ±2g / 4096 LSB/g
 *      [CAL  ] 静止校准中：请勿移动板子（约 0.5 秒）...
 *      [CAL  ] 样本 100：均值=(+0.145,+0.288,-0.899) g  原始 |a|=0.9556 g  波动 0.0078 g
 *      [CAL  ] ✅ 校准完成：增益=1.04646（0.9556 g → 1.0000 g），静止时 |a| 应稳定在 1.000 g 附近
 *      [READY] 【离线模式】只读传感器 + 串口打印：不连 WiFi、不校时、不上报
 *      [DATA] #1  x=+0.152  y=+0.301  z=-0.941 g   |a|=1.0000 g   roll=+162.3  pitch=  -9.1 deg   静止 (|a|≈1g，可直接当基准比对)
 *      [STAT] 最近 10 条：|a| 最小 0.9987  最大 1.0015  平均 1.0001 g
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>

#include "config.h"

// 运行模式开关（在 config.h 里定义；没定义时默认走联网上传）
//   ENABLE_UPLOAD = 1 → 联网模式：连 WiFi + NTP 校时 + HTTP 上报
//   ENABLE_UPLOAD = 0 → 离线模式：只读传感器 + 串口打印，不联网、不校时、不上报
#ifndef ENABLE_UPLOAD
#define ENABLE_UPLOAD 1
#endif

// 静止自动校准开关（在 config.h 里定义；没定义时默认开启）
#ifndef ENABLE_AUTO_CALIB
#define ENABLE_AUTO_CALIB 1
#endif

// ============================ 传感器类型 ============================
enum SensorKind {
  SENSOR_NONE = 0,
  SENSOR_INVENSENSE,    // MPU6050 家族：三轴加速度 + 三轴陀螺仪
  SENSOR_ST_LSM6,       // ST LSM6 家族：三轴加速度 + 三轴陀螺仪
  SENSOR_BMI160,        // Bosch BMI160：三轴加速度 + 三轴陀螺仪
  SENSOR_QMI8658,       // QMI8658：三轴加速度 + 三轴陀螺仪
  SENSOR_QMA6100P,      // QMA6100P @0x12，ID=0x90（无陀螺仪）
  SENSOR_QMA7981,       // QMA7981  @0x12，ID=0xE7（无陀螺仪）
  SENSOR_ADXL345        // 三轴加速度计（无陀螺仪）
};

struct ImuSample {
  float ax, ay, az;         // 单位 g
  float gx, gy, gz;         // 单位 dps (度/秒)
  float accelMag;           // 合矢量 |a|
  float gyroMag;            // 合矢量 |ω|
  bool  gyroValid;
};

struct ImuDev {
  SensorKind kind     = SENSOR_NONE;
  uint8_t    addr     = 0;
  const char *name    = "none";
  bool       hasGyro  = false;
  float      accelLsbPerG  = 0.0f;   // 1 g 对应多少 LSB
  float      gyroLsbPerDps = 0.0f;   // 1 dps 对应多少 LSB
};

static ImuDev g_imu;

// ============================ 运行状态 ============================
static bool     g_sampling     = true;
static uint32_t g_seq          = 0;
static uint32_t g_lastSampleMs = 0;
static uint32_t g_lastUploadMs = 0;
static uint32_t g_lastDetectMs = 0;
static uint32_t g_okCount      = 0;
static uint32_t g_failCount    = 0;
static bool     g_buttonPrev   = true;
static uint32_t g_buttonDownMs = 0;
static ImuSample g_last;

// 静止校准系数（readImu 里要用，故声明在这里）
static float g_calibGain = 1.0f;    // 三轴统一缩放因子
static bool  g_calibOk   = false;   // 是否已完成校准

// 最近一次 I2C 扫描结果（供探针诊断复用）
static uint8_t g_scanAddrs[16];
static int     g_scanCount = 0;

// ============================ 工具 ============================
static void logTag(const char *tag, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%-5s] %s\n", tag, buf);
}

static bool i2cReadBytes(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool i2cWriteByte(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static inline int16_t i16be(const uint8_t *b) { return (int16_t)((b[0] << 8) | b[1]); }
static inline int16_t i16le(const uint8_t *b) { return (int16_t)((b[1] << 8) | b[0]); }

// ============================ I2C 扫描 ============================
static void scanI2CBus() {
  char line[192];
  int n = 0;
  g_scanCount = 0;
  line[0] = '\0';

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char tmp[12];
      snprintf(tmp, sizeof(tmp), "0x%02X ", addr);
      if (strlen(line) + strlen(tmp) < sizeof(line) - 1) strcat(line, tmp);
      if (g_scanCount < (int)(sizeof(g_scanAddrs) / sizeof(g_scanAddrs[0])))
        g_scanAddrs[g_scanCount++] = addr;
      n++;
    }
  }
  if (n == 0) logTag("I2C", "总线上没有发现任何设备（检查接线 / 上拉 / 供电 / 引脚号）");
  else        logTag("I2C", "扫描到 %d 个设备: %s", n, line);
}

// 刚才扫描时这个地址应答过吗？
// 作用：避免对"根本不存在的地址"发起读操作 —— ESP32 的 Wire 在重复起始读失败时
// 会打出一堆 `i2cWriteReadNonStop returned Error -1`，属于正常现象但很刷屏
static bool addrSeen(uint8_t addr) {
  for (int i = 0; i < g_scanCount; i++)
    if (g_scanAddrs[i] == addr) return true;
  return false;
}

// 诊断探针：对扫描到的每个地址，读几个常见的 WHO_AM_I 寄存器
// 用途：当所有已知驱动都没命中时，把这张表贴给老师/助教，一眼就能看出是什么芯片
static void probeUnknown() {
  if (g_scanCount == 0) return;
  logTag("PROBE", "未知设备探针（WHO_AM_I 取值表）:");
  for (int i = 0; i < g_scanCount; i++) {
    uint8_t a = g_scanAddrs[i];
    uint8_t v75 = 0, v0f = 0, v00 = 0;
    bool ok75 = i2cReadBytes(a, 0x75, &v75, 1);
    bool ok0f = i2cReadBytes(a, 0x0F, &v0f, 1);
    bool ok00 = i2cReadBytes(a, 0x00, &v00, 1);
    char s75[8], s0f[8], s00[8];
    if (ok75) snprintf(s75, sizeof(s75), "0x%02X", v75); else snprintf(s75, sizeof(s75), "--");
    if (ok0f) snprintf(s0f, sizeof(s0f), "0x%02X", v0f); else snprintf(s0f, sizeof(s0f), "--");
    if (ok00) snprintf(s00, sizeof(s00), "0x%02X", v00); else snprintf(s00, sizeof(s00), "--");
    logTag("PROBE", "  addr=0x%02X  reg0x75=%s  reg0x0F=%s  reg0x00=%s",
           a, s75, s0f, s00);
  }
  logTag("PROBE", "  对照：MPU6050=75:68  MPU6500=75:70  MPU9250=75:71  MPU6886=75:19");
  logTag("PROBE", "        ICM-20602=75:12  LSM6DS3=0F:69  LSM6DSO=0F:6C  BMI160=00:D1");
  logTag("PROBE", "        QMI8658=00:05   QMA6100P=00:90   QMA7981=00:E7（后两个无陀螺仪）");
}

// ============================ IMU 驱动 ============================
// 安装成功时统一填好 g_imu
static void installImu(SensorKind kind, uint8_t addr, const char *name,
                       bool hasGyro, float accelLsbPerG, float gyroLsbPerDps) {
  g_imu.kind           = kind;
  g_imu.addr           = addr;
  g_imu.name           = name;
  g_imu.hasGyro        = hasGyro;
  g_imu.accelLsbPerG   = accelLsbPerG;
  g_imu.gyroLsbPerDps  = gyroLsbPerDps;
}

// ---- InvenSense 家族（MPU6000/6050/6500/9250/9255/6886, ICM-20602/20608-G/20689）
//      寄存器布局彼此兼容，地址 0x68 或 0x69
static bool initInvenSense() {
  for (uint8_t addr = 0x68; addr <= 0x69; addr++) {
    if (!addrSeen(addr)) continue;
    uint8_t who = 0;
    if (!i2cReadBytes(addr, 0x75, &who, 1)) continue;

    const char *model = nullptr;
    switch (who) {
      case 0x68: model = "MPU6050/MPU6000"; break;
      case 0x70: model = "MPU6500";         break;
      case 0x71: model = "MPU9250";         break;
      case 0x73: model = "MPU9255";         break;
      case 0x19: model = "MPU6886";         break;
      case 0x12: model = "ICM-20602";       break;
      case 0xAE: model = "ICM-20608-G";     break;
      case 0x98: model = "ICM-20689";       break;
      default:   continue;
    }

    i2cWriteByte(addr, 0x6B, 0x01);   // PWR_MGMT_1：唤醒，时钟选陀螺仪 X 轴 PLL
    i2cWriteByte(addr, 0x1A, 0x03);   // CONFIG：DLPF ≈41Hz
    i2cWriteByte(addr, 0x1C, 0x00);   // ACCEL_CONFIG：±2g   → 16384 LSB/g
    i2cWriteByte(addr, 0x1B, 0x00);   // GYRO_CONFIG ：±250dps → 131 LSB/dps
    delay(50);

    installImu(SENSOR_INVENSENSE, addr, model, true, 16384.0f, 131.0f);
    logTag("IMU", "命中 %s @0x%02X (WHO_AM_I=0x%02X)：三轴加速度 + 三轴陀螺仪  ±2g / ±250dps",
           model, addr, who);
    return true;
  }
  return false;
}

// ---- ST 家族（LSM6DS3 / LSM6DSL / LSM6DS3TR-C / LSM6DSO / ISM330DLC / ASM330LHH / LSM6DSV）
//      WHO_AM_I 在 0x0F，数据小端，地址 0x6A 或 0x6B
static bool initStLsm6() {
  for (uint8_t addr = 0x6A; addr <= 0x6B; addr++) {
    if (!addrSeen(addr)) continue;
    uint8_t who = 0;
    if (!i2cReadBytes(addr, 0x0F, &who, 1)) continue;

    const char *model = nullptr;
    switch (who) {
      case 0x69: model = "LSM6DS3";            break;
      case 0x6A: model = "LSM6DSL/LSM6DS3TR-C"; break;
      case 0x6B: model = "ISM330DLC/ASM330LHH"; break;
      case 0x6C: model = "LSM6DSO/LSM6DSO32";   break;
      case 0x70: model = "LSM6DSV";             break;
      default:   continue;
    }

    i2cWriteByte(addr, 0x12, 0x44);   // CTRL3_C：BDU + IF_INC（寄存器自动递增）
    i2cWriteByte(addr, 0x10, 0x60);   // CTRL1_XL：416Hz，±2g   → 16393 LSB/g
    i2cWriteByte(addr, 0x11, 0x60);   // CTRL2_G ：416Hz，±245dps → 114.3 LSB/dps
    delay(50);

    installImu(SENSOR_ST_LSM6, addr, model, true, 16393.0f, 114.29f);
    logTag("IMU", "命中 %s @0x%02X (WHO_AM_I=0x%02X)：三轴加速度 + 三轴陀螺仪  ±2g / ±245dps",
           model, addr, who);
    return true;
  }
  return false;
}

// ---- Bosch BMI160：CHIP_ID(0x00)=0xD1，地址 0x68/0x69
static bool initBmi160() {
  for (uint8_t addr = 0x68; addr <= 0x69; addr++) {
    if (!addrSeen(addr)) continue;
    uint8_t id = 0;
    if (!i2cReadBytes(addr, 0x00, &id, 1) || id != 0xD1) continue;

    i2cWriteByte(addr, 0x7E, 0x11);   // CMD：加速度计 → normal
    i2cWriteByte(addr, 0x7E, 0x15);   // CMD：陀螺仪   → normal
    delay(10);
    i2cWriteByte(addr, 0x41, 0x03);   // ACC_RANGE：±2g     → 16384 LSB/g
    i2cWriteByte(addr, 0x40, 0x28);   // ACC_CONF ：100Hz
    i2cWriteByte(addr, 0x43, 0x00);   // GYR_RANGE：±2000dps → 16.384 LSB/dps
    i2cWriteByte(addr, 0x42, 0x28);   // GYR_CONF ：100Hz
    delay(50);

    installImu(SENSOR_BMI160, addr, "BMI160", true, 16384.0f, 16.384f);
    logTag("IMU", "命中 BMI160 @0x%02X (CHIP_ID=0xD1)：三轴加速度 + 三轴陀螺仪  ±2g / ±2000dps",
           addr);
    return true;
  }
  return false;
}

// ---- QMI8658：WHO_AM_I(0x00)=0x05，地址 0x6A/0x6B
static bool initQmi8658() {
  for (uint8_t addr = 0x6A; addr <= 0x6B; addr++) {
    if (!addrSeen(addr)) continue;
    uint8_t who = 0;
    if (!i2cReadBytes(addr, 0x00, &who, 1) || who != 0x05) continue;

    i2cWriteByte(addr, 0x60, 0xB0);   // RESET：软复位，回到已知状态
    delay(15);
    i2cWriteByte(addr, 0x02, 0x40);   // CTRL1：bit6=ADDR_AI 地址自动递增（保持小端）
    i2cWriteByte(addr, 0x03, 0x06);   // CTRL2：aFS=±2g，aODR=125Hz → 16384 LSB/g
    i2cWriteByte(addr, 0x04, 0x46);   // CTRL3：gFS=±256dps，gODR=125Hz → 128 LSB/dps
    i2cWriteByte(addr, 0x06, 0x00);   // CTRL5：关闭内置低通滤波，保留原始数据
    i2cWriteByte(addr, 0x08, 0x03);   // CTRL7：同时使能 加速度计 + 陀螺仪
    delay(30);

    installImu(SENSOR_QMI8658, addr, "QMI8658", true, 16384.0f, 128.0f);
    logTag("IMU", "命中 QMI8658 @0x%02X (WHO_AM_I=0x05)：三轴加速度 + 三轴陀螺仪  ±2g / ±256dps",
           addr);
    return true;
  }
  return false;
}

// ---- QMA6100P：地址 0x12，芯片 ID 0x90（QMA7981 的 pin-to-pin 兼容型号）
//      寄存器：0x00=ID, 0x01~0x06=XYZ 数据(小端,14 位), 0x0F=量程, 0x10=带宽,
//              0x11=电源, 0x36=软复位(写 0xB6)
static bool initQma6100p() {
  if (!addrSeen(0x12)) return false;
  uint8_t id = 0;
  if (!i2cReadBytes(0x12, 0x00, &id, 1) || id != 0x90) return false;

  i2cWriteByte(0x12, 0x36, 0xB6);   // RESET：软复位
  delay(5);
  i2cWriteByte(0x12, 0x36, 0x00);   // 结束复位
  delay(10);
  i2cWriteByte(0x12, 0x11, 0x80);   // POWER_MANAGE：进入主动模式
  i2cWriteByte(0x12, 0x11, 0x84);   // POWER_MANAGE：MCLK 51.2kHz + 主动模式
  // 下面三行是模拟前端的校准序列，来自芯片官方例程，顺序和值都不能改
  i2cWriteByte(0x12, 0x4A, 0x20);   // TST0_ANA
  i2cWriteByte(0x12, 0x56, 0x01);   // AFE_ANA
  i2cWriteByte(0x12, 0x5F, 0x80);   // TST1_ANA
  delay(1);
  i2cWriteByte(0x12, 0x5F, 0x00);
  delay(10);
  i2cWriteByte(0x12, 0x0F, 0x01);   // RANGE：±2g  → 4096 LSB/g
  i2cWriteByte(0x12, 0x10, 0x00);   // BW_ODR：100Hz
  delay(20);

  installImu(SENSOR_QMA6100P, 0x12, "QMA6100P", false, 4096.0f, 0.0f);
  logTag("IMU", "命中 QMA6100P @0x12（只有三轴加速度，无陀螺仪）  ±2g / 4096 LSB/g");
  return true;
}

// ---- QMA7981：地址 0x12，芯片 ID 0xE7。寄存器布局与 QMA6100P 基本一致
static bool initQMA7981() {
  if (!addrSeen(0x12)) return false;
  uint8_t id = 0;
  if (!i2cReadBytes(0x12, 0x00, &id, 1) || id != 0xE7) return false;

  i2cWriteByte(0x12, 0x36, 0xB6);   // 上电复位
  delay(5);
  i2cWriteByte(0x12, 0x36, 0x00);
  delay(10);
  i2cWriteByte(0x12, 0x11, 0xC0);   // PMU_LPW：进入 Active 模式
  i2cWriteByte(0x12, 0x0F, 0x01);   // PMU_RANGE：±2g  → 4096 LSB/g
  i2cWriteByte(0x12, 0x10, 0x05);   // PMU_BW：带宽 ≈1kHz
  delay(20);

  installImu(SENSOR_QMA7981, 0x12, "QMA7981", false, 4096.0f, 0.0f);
  logTag("IMU", "命中 QMA7981 @0x12（只有三轴加速度，无陀螺仪）  ±2g / 4096 LSB/g");
  return true;
}

// ---- ADXL345：外接三轴加速度计，没有陀螺仪
static bool initADXL345() {
  if (!addrSeen(0x53)) return false;
  uint8_t id = 0;
  if (!i2cReadBytes(0x53, 0x00, &id, 1) || id != 0xE5) return false;

  i2cWriteByte(0x53, 0x31, 0x0B);   // DATA_FORMAT：全分辨率 ±16g，3.9mg/LSB
  i2cWriteByte(0x53, 0x2D, 0x08);   // POWER_CTL：进入测量模式
  delay(20);

  installImu(SENSOR_ADXL345, 0x53, "ADXL345", false, 256.0f, 0.0f);
  logTag("IMU", "命中 ADXL345 @0x53（只有三轴加速度，无陀螺仪）  全分辨率 ±16g");
  return true;
}

// 自动识别。**带陀螺仪的六轴芯片优先**，只有找不到六轴时才退回纯加速度计
static bool detectImu() {
  logTag("IMU", "开始自动识别 IMU（优先找带陀螺仪的六轴）...");

  if (initInvenSense()) return true;   // 六轴
  if (initStLsm6())     return true;   // 六轴
  if (initBmi160())     return true;   // 六轴
  if (initQmi8658())    return true;   // 六轴
  if (initQma6100p())   return true;   // 仅加速度（0x12, ID 0x90）
  if (initQMA7981())    return true;   // 仅加速度（0x12, ID 0xE7）
  if (initADXL345())    return true;   // 仅加速度

  g_imu.kind    = SENSOR_NONE;
  g_imu.name    = "none";
  g_imu.hasGyro = false;
  logTag("IMU", "❌ 没识别到 IMU！本工程只做 IMU，不做替代数据源。");
  logTag("IMU", "   排查顺序：① 供电 3.3V ② SDA/SCL 是否接反 ③ 引脚号是否与 config.h 一致");
  logTag("IMU", "   ④ 若把外接 IMU 接在 ESP32-S3-EYE 上，注意板载 QMA7981 也在同一条 I2C 上");
  probeUnknown();
  logTag("IMU", "   5 秒后重新扫描 I2C ...");
  return false;
}

// 读取一次 IMU。返回 false 表示 I2C 读取失败
static bool readImu(ImuSample &s) {
  uint8_t b[12] = {0};

  switch (g_imu.kind) {
    case SENSOR_INVENSENSE: {
      if (!i2cReadBytes(g_imu.addr, 0x3B, b, 6)) return false;   // ACCEL_XOUT_H ...
      s.ax = i16be(b + 0) / g_imu.accelLsbPerG;
      s.ay = i16be(b + 2) / g_imu.accelLsbPerG;
      s.az = i16be(b + 4) / g_imu.accelLsbPerG;
      if (!i2cReadBytes(g_imu.addr, 0x43, b, 6)) return false;   // GYRO_XOUT_H ...
      s.gx = i16be(b + 0) / g_imu.gyroLsbPerDps;
      s.gy = i16be(b + 2) / g_imu.gyroLsbPerDps;
      s.gz = i16be(b + 4) / g_imu.gyroLsbPerDps;
      s.gyroValid = true;
      break;
    }

    case SENSOR_ST_LSM6: {
      // OUTX_L_G(0x22) 起 12 字节：陀螺仪 6 字节 + 加速度 6 字节，小端
      if (!i2cReadBytes(g_imu.addr, 0x22, b, 12)) return false;
      s.gx = i16le(b + 0) / g_imu.gyroLsbPerDps;
      s.gy = i16le(b + 2) / g_imu.gyroLsbPerDps;
      s.gz = i16le(b + 4) / g_imu.gyroLsbPerDps;
      s.ax = i16le(b + 6) / g_imu.accelLsbPerG;
      s.ay = i16le(b + 8) / g_imu.accelLsbPerG;
      s.az = i16le(b + 10) / g_imu.accelLsbPerG;
      s.gyroValid = true;
      break;
    }

    case SENSOR_BMI160: {
      if (!i2cReadBytes(g_imu.addr, 0x0C, b, 6)) return false;   // GYR_X_L ...
      s.gx = i16le(b + 0) / g_imu.gyroLsbPerDps;
      s.gy = i16le(b + 2) / g_imu.gyroLsbPerDps;
      s.gz = i16le(b + 4) / g_imu.gyroLsbPerDps;
      if (!i2cReadBytes(g_imu.addr, 0x12, b, 6)) return false;   // ACC_X_L ...
      s.ax = i16le(b + 0) / g_imu.accelLsbPerG;
      s.ay = i16le(b + 2) / g_imu.accelLsbPerG;
      s.az = i16le(b + 4) / g_imu.accelLsbPerG;
      s.gyroValid = true;
      break;
    }

    case SENSOR_QMI8658: {
      // 0x35 起 12 字节：加速度 6 字节 + 陀螺仪 6 字节，小端
      if (!i2cReadBytes(g_imu.addr, 0x35, b, 12)) return false;
      s.ax = i16le(b + 0) / g_imu.accelLsbPerG;
      s.ay = i16le(b + 2) / g_imu.accelLsbPerG;
      s.az = i16le(b + 4) / g_imu.accelLsbPerG;
      s.gx = i16le(b + 6) / g_imu.gyroLsbPerDps;
      s.gy = i16le(b + 8) / g_imu.gyroLsbPerDps;
      s.gz = i16le(b + 10) / g_imu.gyroLsbPerDps;
      s.gyroValid = true;
      break;
    }

    // QMA6100P / QMA7981 数据布局相同：XOUTL(0x01) 起 6 字节，小端，14 位左对齐
    case SENSOR_QMA6100P:
    case SENSOR_QMA7981: {
      if (!i2cReadBytes(g_imu.addr, 0x01, b, 6)) return false;
      s.ax = (i16le(b + 0) >> 2) / g_imu.accelLsbPerG;   // 右移 2 位完成符号扩展
      s.ay = (i16le(b + 2) >> 2) / g_imu.accelLsbPerG;
      s.az = (i16le(b + 4) >> 2) / g_imu.accelLsbPerG;
      s.gx = s.gy = s.gz = 0.0f;
      s.gyroValid = false;
      break;
    }

    case SENSOR_ADXL345: {
      if (!i2cReadBytes(g_imu.addr, 0x32, b, 6)) return false;   // DATAX0 ...（小端）
      s.ax = i16le(b + 0) / g_imu.accelLsbPerG;
      s.ay = i16le(b + 2) / g_imu.accelLsbPerG;
      s.az = i16le(b + 4) / g_imu.accelLsbPerG;
      s.gx = s.gy = s.gz = 0.0f;
      s.gyroValid = false;
      break;
    }

    default:
      return false;
  }

  // 应用静止校准（三轴统一缩放，不改变读数方向）
  if (g_calibOk) {
    s.ax *= g_calibGain;
    s.ay *= g_calibGain;
    s.az *= g_calibGain;
  }

  s.accelMag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
  s.gyroMag  = s.gyroValid ? sqrtf(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz) : 0.0f;
  return true;
}

// ============================ 静止自动校准 ============================
// MEMS 加速度计有出厂零点/灵敏度误差，静止时 |a| 常常不是标准 1.000 g。
// 做法：开机后让板子静止，采 N 个样本求均值向量 → 得到一个比例因子，
//       使静止时 |a| 归一到 1.0000 g（三轴统一缩放，不改变读数方向）。
// 为什么用"比例"而不是"逐轴零点"：单次静止观测只能解出"模长"这一个约束，
//       逐轴零点需要多姿态数据（六面法）。比例校准对"当堂核对 |a|≈1g"最有效。
static void calibrateStatic() {
  const int   WARMUP   = 30;              // 上电初期总线/寄存器未就绪，先丢弃这些样本
  const int   N        = 100;             // 用于统计的样本数
  const float GAIN_OLD = g_calibGain;     // 旧系数，用来把读数还原成"原始模长"

  double sx = 0, sy = 0, sz = 0;
  float mn = 1e9f, mx = -1e9f;
  int good = 0;

  logTag("CAL ", "静止校准中：请勿移动板子（约 %.1f 秒）...", (WARMUP + N) * 5.0f / 1000.0f);

  for (int i = 0; i < WARMUP; i++) {      // 预热轮：读但不计入统计
    ImuSample s;
    readImu(s);
    delay(5);
  }

  for (int i = 0; i < N; i++) {
    ImuSample s;
    if (readImu(s)) {
      float rawMag = s.accelMag / GAIN_OLD;                  // 还原该样本的原始模长
      if (rawMag < 0.5f || rawMag > 1.5f) { delay(5); continue; }  // 剔除毛刺/垃圾样本
      sx += s.ax; sy += s.ay; sz += s.az;
      if (rawMag < mn) mn = rawMag;
      if (rawMag > mx) mx = rawMag;
      good++;
    }
    delay(5);
  }

  if (good < N * 3 / 5) {
    logTag("CAL ", "❌ 校准失败：有效样本太少（%d/%d），沿用原系数 %.5f；检查传感器是否松动",
           good, N, g_calibGain);
    return;
  }

  // readImu 已把旧系数算进去了，所以读到的 mag 是"缩放后"的模长；
  // 还原出原始模长再给出新的绝对系数 —— 这样反复校准也不会累积漂移。
  float ax = sx / good, ay = sy / good, az = sz / good;
  float mag     = sqrtf(ax * ax + ay * ay + az * az);
  float rawMag  = mag / GAIN_OLD;      // 未经校准的原始模长
  float spread  = mx - mn;

  logTag("CAL ", "样本 %d：均值=(%+.4f,%+.4f,%+.4f) g  原始 |a|=%.4f g  波动 %.4f g",
         good, ax, ay, az, rawMag, spread);

  if (rawMag < 0.5f || rawMag > 1.5f) {
    logTag("CAL ", "❌ 校准失败：原始 |a|=%.4f g 明显异常，检查传感器是否故障/松动", rawMag);
    return;
  }
  if (spread > 0.10f) {
    logTag("CAL ", "⚠️  校准时板子在动（波动 %.4f > 0.10 g），结果可能不准；放稳后长按 BOOT 重校", spread);
  }

  g_calibGain = GAIN_OLD / mag;        // 使 原始模长 × 新系数 = 1.0000
  g_calibOk   = true;
  logTag("CAL ", "✅ 校准完成：增益=%.5f（%.4f g → 1.0000 g），静止时 |a| 应稳定在 1.000 g 附近",
         g_calibGain, rawMag);
}

// ============================ 网络（仅联网模式编译）============================
#if ENABLE_UPLOAD
static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  logTag("WIFI", "正在连接 ssid=\"%s\" ...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    logTag("WIFI", "已连接  ip=%s  rssi=%d dBm",
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    logTag("WIFI", "连接失败（20s 超时）。检查 SSID/密码、是否为 2.4GHz、信号强度");
  }
}

static void syncTime() {
  configTime(NTP_TZ_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
  uint32_t t0 = millis();
  while (millis() - t0 < 10000) {
    time_t now = time(nullptr);
    if (now > 1700000000) {              // > 2023-11 视为 NTP 成功
      logTag("TIME", "NTP 对准，板端 UTC = %ld", (long)now);
      return;
    }
    delay(300);
  }
  logTag("TIME", "NTP 未对准，将上报 ts_device=0；页面会显示板端时间未知");
}

// 上报一条记录。返回 HTTP 状态码，<0 表示网络层失败
static int uploadRecord(const ImuSample &s) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  time_t nowUtc = time(nullptr);
  long tsDevice = (nowUtc > 1700000000) ? (long)nowUtc : 0;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // 主指标固定为 accel_mag（静止时应 ≈1.000 g，与板子朝向无关，最适合当堂核对）
  // 六轴分量放进 axes，页面可以切换曲线查看
  char body[768];

  if (s.gyroValid) {
    snprintf(body, sizeof(body),
      "{\"device_id\":\"%s\",\"seq\":%lu,"
      "\"metric\":\"accel_mag\",\"value\":%.4f,\"unit\":\"g\","
      "\"axes\":{"
        "\"accel\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"unit\":\"g\"},"
        "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"unit\":\"dps\"}"
      "},"
      "\"sensor\":\"%s\",\"has_gyro\":true,"
      "\"ts_device\":%ld,\"uptime_ms\":%lu,\"rssi\":%d,\"mac\":\"%s\","
      "\"fw\":\"%s\",\"sampling\":%s}",
      DEVICE_ID, (unsigned long)g_seq, s.accelMag,
      s.ax, s.ay, s.az,
      s.gx, s.gy, s.gz,
      g_imu.name,
      tsDevice, (unsigned long)millis(), (int)WiFi.RSSI(), macStr,
      FW_VERSION, g_sampling ? "true" : "false");
  } else {
    snprintf(body, sizeof(body),
      "{\"device_id\":\"%s\",\"seq\":%lu,"
      "\"metric\":\"accel_mag\",\"value\":%.4f,\"unit\":\"g\","
      "\"axes\":{"
        "\"accel\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"unit\":\"g\"},"
        "\"gyro\":null"
      "},"
      "\"sensor\":\"%s\",\"has_gyro\":false,"
      "\"ts_device\":%ld,\"uptime_ms\":%lu,\"rssi\":%d,\"mac\":\"%s\","
      "\"fw\":\"%s\",\"sampling\":%s}",
      DEVICE_ID, (unsigned long)g_seq, s.accelMag,
      s.ax, s.ay, s.az,
      g_imu.name,
      tsDevice, (unsigned long)millis(), (int)WiFi.RSSI(), macStr,
      FW_VERSION, g_sampling ? "true" : "false");
  }

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.begin(SERVER_INGEST_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  int code = http.POST((uint8_t *)body, strlen(body));
  http.end();
  return code;
}
#endif   // ENABLE_UPLOAD

// ============================ 按键：暂停 / 恢复 ============================
static void pollButton() {
  bool now = digitalRead(BUTTON_PIN);        // 按下 = LOW
  if (g_buttonPrev && !now) g_buttonDownMs = millis();
  if (!g_buttonPrev && now) {                // 松开沿
    uint32_t held = millis() - g_buttonDownMs;
    if (held > 40) {                         // 简易消抖
      if (held >= 1200) {
        calibrateStatic();                   // 长按 ≥1.2 秒：重新做静止校准
      } else {
        g_sampling = !g_sampling;
        if (g_sampling) logTag("PAUSE", "采样已恢复");
        else logTag("PAUSE", "采样已暂停（旧记录保留，页面应提示\"数据未更新\"）");
      }
    }
  }
  g_buttonPrev = now;
}

// ============================ I2C 诊断模式 ============================
// 用 -DI2C_DIAG=1 编译（platformio.ini 里的 diag 环境）时会进入这里：
// 在 100kHz / 400kHz 两种速度下扫描总线，并把每个设备的 0x00~0x7F 寄存器全部打印出来。
// 目的：这类"芯片 ID 对不上"的问题，靠猜没意义，把寄存器表 dump 出来一眼就能认出芯片。
#if defined(I2C_DIAG)
static void runI2cDiag() {
  const uint32_t freqs[2] = {100000, 400000};

  Serial.println();
  Serial.println("############ I2C 诊断开始 ############");
  Serial.printf("引脚 SDA=GPIO%d  SCL=GPIO%d\n", I2C_SDA_PIN, I2C_SCL_PIN);

  for (int f = 0; f < 2; f++) {
    Wire.end();
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, freqs[f]);
    delay(100);

    Serial.printf("\n---- 总线频率 %lu Hz ----\n", (unsigned long)freqs[f]);

    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      uint8_t rc = Wire.endTransmission();          // 带 STOP，能可靠判断 NACK
      if (rc != 0) continue;
      found++;
      Serial.printf(">>> 设备 0x%02X 应答 (endTransmission rc=0)\n", a);

      // 逐寄存器读取 0x00~0x7F
      for (int row = 0; row < 0x80; row += 16) {
        Serial.printf("    0x%02X :", row);
        for (int c = 0; c < 16; c++) {
          uint8_t r = (uint8_t)(row + c);
          uint8_t v = 0;
          // 先试"重复起始"读法，失败再试"带 STOP"的读法
          bool ok = i2cReadBytes(a, r, &v, 1);
          if (ok) {
            Serial.printf(" %02X", v);
          } else {
            Wire.beginTransmission(a);
            Wire.write(r);
            if (Wire.endTransmission() == 0 && Wire.requestFrom((int)a, 1) == 1) {
              v = (uint8_t)Wire.read();
              Serial.printf(" %02X", v);
            } else {
              Serial.print(" ??");
            }
          }
        }
        Serial.println();
      }
    }
    if (!found) Serial.println("(本频率下总线上没有任何设备应答)");
  }

  Serial.println("\n############ 诊断结束，停在死循环 ############");
  while (true) {
    delay(2000);
    Serial.println("[diag] 仍在运行（重新烧录正常固件即可退出）");
  }
}
#endif

// ============================ 本地采样打印（离线模式的核心输出）============================
// 完全不需要网络：每秒读一次真实传感器，把三轴、合矢量、倾角打到串口，
// 并每 10 条给一次统计 —— 这就是"能真实跑起来"的最小闭环。
static uint32_t g_statCount = 0;
static float    g_statMin   =  1e9f;
static float    g_statMax   = -1e9f;
static float    g_statSum   =  0.0f;

static void printSample(const ImuSample &s) {
  const float PI_F = 3.14159265f;
  // 用重力方向反推倾角（静态下才准；加速度计无法直接测偏航角 yaw）
  float roll  = atan2f(s.ay, s.az) * 180.0f / PI_F;
  float pitch = atan2f(-s.ax, sqrtf(s.ay * s.ay + s.az * s.az)) * 180.0f / PI_F;

  const char *state;
  if (fabsf(s.accelMag - 1.0f) <= 0.05f) state = "静止 (|a|≈1g，可直接当基准比对)";
  else if (s.accelMag < 0.5f)            state = "疑似自由落体/失重";
  else                                   state = "有运动";

  Serial.printf("[DATA] #%lu  x=%+.3f  y=%+.3f  z=%+.3f g   |a|=%.4f g   roll=%+6.1f  pitch=%+6.1f deg   %s\n",
                (unsigned long)g_seq, s.ax, s.ay, s.az, s.accelMag, roll, pitch, state);

  g_statCount++;
  if (s.accelMag < g_statMin) g_statMin = s.accelMag;
  if (s.accelMag > g_statMax) g_statMax = s.accelMag;
  g_statSum += s.accelMag;
  if (g_statCount >= 10) {
    Serial.printf("[STAT] 最近 %lu 条：|a| 最小 %.4f  最大 %.4f  平均 %.4f g\n",
                  (unsigned long)g_statCount, g_statMin, g_statMax, g_statSum / g_statCount);
    g_statCount = 0;
    g_statMin = 1e9f;
    g_statMax = -1e9f;
    g_statSum = 0.0f;
  }
}

// ============================ WiFi 扫描模式 ============================
// 用 -DWIFI_SCAN=1 编译（platformio.ini 里的 wifiscan 环境）时进入这里：
// 打印板子实际能搜到的所有 AP，并标出 5G 网络（ESP32 连不上 5G）
#if defined(WIFI_SCAN)
static void runWifiScan() {
  Serial.println();
  Serial.println("########## WiFi 扫描 ##########");
  Serial.printf("配置里的 SSID = \"%s\"\n\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);

  int n = WiFi.scanNetworks();
  Serial.printf("板子搜到 %d 个网络：\n", n);
  bool found = false;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s == WIFI_SSID) found = true;
    int ch = WiFi.channel(i);
    Serial.printf("  %2d) ssid=\"%s\"  rssi=%d dBm  ch=%d  %s\n",
                  i + 1, s.c_str(), (int)WiFi.RSSI(i), ch,
                  ch <= 14 ? "2.4G ✅" : "5G ❌(ESP32 连不上)");
  }
  Serial.printf("\n结论：配置的 SSID 在扫描结果里 %s\n",
                found ? "✅ 找到了 —— 那就是密码/加密方式的问题"
                      : "❌ 没找到 —— 名字写错了，或者根本不是 2.4G 网络");

  Serial.println("\n########## 扫描结束，停在死循环 ##########");
  while (true) {
    delay(3000);
    Serial.println("[wifiscan] 仍在运行（重新烧录正常固件即可退出）");
  }
}
#endif

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("==================================================");

#if defined(WIFI_SCAN)
  logTag("BOOT", "fw=%s 模式=WiFi 扫描", FW_VERSION);
  runWifiScan();         // 永不返回
#endif

#if defined(I2C_DIAG)
  logTag("BOOT", "fw=%s 模式=I2C 诊断", FW_VERSION);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  runI2cDiag();          // 永不返回
#endif

#if defined(BOARD_PROFILE_ESP32S3EYE)
  logTag("BOOT", "fw=%s board=ESP32-S3-EYE dev=%s", FW_VERSION, DEVICE_ID);
#elif defined(BOARD_PROFILE_ESPEYE)
  logTag("BOOT", "fw=%s board=ESP-EYE/ESP32 dev=%s", FW_VERSION, DEVICE_ID);
#else
  logTag("BOOT", "fw=%s board=unknown dev=%s", FW_VERSION, DEVICE_ID);
#endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // 关掉"探测不存在的 I2C 地址"时的报错刷屏（那是正常现象，不是故障）
  esp_log_level_set("Wire", ESP_LOG_NONE);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  scanI2CBus();
  detectImu();

#if ENABLE_AUTO_CALIB
  if (g_imu.kind != SENSOR_NONE) calibrateStatic();
#else
  logTag("CAL ", "自动校准已关闭（config.h 里 ENABLE_AUTO_CALIB=0），使用原始读数");
#endif

#if ENABLE_UPLOAD
  connectWiFi();
  syncTime();
  logTag("READY", "【联网模式】开始采样并上报，服务端 = %s", SERVER_INGEST_URL);
#else
  logTag("READY", "【离线模式】只读传感器 + 串口打印：不连 WiFi、不校时、不上报");
  logTag("READY", "  要改成联网上传 → 把 config.h 里的 ENABLE_UPLOAD 改成 1，重新烧录");
#endif
  logTag("READY", "按键：短按 BOOT(GPIO%d) 暂停/恢复采样；长按 ≥1.2s 重新静止校准", BUTTON_PIN);

  g_lastSampleMs = millis();
  g_lastUploadMs = millis();
}

void loop() {
  uint32_t now = millis();

  pollButton();

#if ENABLE_UPLOAD
  // WiFi 掉线自动重连
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastRetry = 0;
    if (now - lastRetry > 5000) {
      lastRetry = now;
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED) syncTime();
    }
  }
#endif

  // 没识别到 IMU：每 5 秒重新扫描一次，方便边插线边看结果
  if (g_imu.kind == SENSOR_NONE) {
    if (now - g_lastDetectMs > 5000) {
      g_lastDetectMs = now;
      scanI2CBus();
      detectImu();
    }
    delay(10);
    return;
  }

  // 采样 + 打印（离线模式就靠这一步产出真实数据）
  if (now - g_lastSampleMs >= SAMPLE_INTERVAL_MS) {
    g_lastSampleMs = now;
    if (g_sampling) {
      ImuSample s;
      if (readImu(s)) {
        g_last = s;
        g_seq++;
        printSample(s);
      } else {
        logTag("IMU", "本次 I2C 读取失败");
      }
    } else {
      // 停采：旧值原样保留，只提示"没有新数据"，不把值清零
      static uint32_t pauseTick = 0;
      if (now - pauseTick >= 3000) {
        pauseTick = now;
        logTag("PAUSE", "采样已暂停：没有新数据，下面是最后一次真实观测");
        Serial.printf("[LAST] #%lu  x=%+.3f  y=%+.3f  z=%+.3f g   |a|=%.4f g\n",
                      (unsigned long)g_seq, g_last.ax, g_last.ay, g_last.az, g_last.accelMag);
      }
    }
  }

#if ENABLE_UPLOAD
  // 上报
  if (g_sampling && now - g_lastUploadMs >= UPLOAD_INTERVAL_MS) {
    g_lastUploadMs = now;
    int code = uploadRecord(g_last);
    if (code == 200 || code == 201) {
      g_okCount++;
      if (g_last.gyroValid)
        logTag("POST", "#%lu accel=(%.3f,%.3f,%.3f)g mag=%.4fg  gyro=(%.1f,%.1f,%.1f)dps  -> HTTP %d (ok=%lu)",
               (unsigned long)g_seq, g_last.ax, g_last.ay, g_last.az, g_last.accelMag,
               g_last.gx, g_last.gy, g_last.gz, code, (unsigned long)g_okCount);
      else
        logTag("POST", "#%lu accel=(%.3f,%.3f,%.3f)g mag=%.4fg  gyro=n/a  -> HTTP %d (ok=%lu)",
               (unsigned long)g_seq, g_last.ax, g_last.ay, g_last.az, g_last.accelMag,
               code, (unsigned long)g_okCount);
    } else {
      g_failCount++;
      logTag("POST", "#%lu 上报失败 code=%d (fail=%lu，已成功 %lu 条)",
             (unsigned long)g_seq, code, (unsigned long)g_failCount, (unsigned long)g_okCount);
    }
  }
#endif   // ENABLE_UPLOAD

  delay(10);
}
