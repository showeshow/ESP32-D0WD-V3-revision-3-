// XPT2046 bit-bang 软 SPI 触摸驱动（CYD 独立引脚 25/32/39/33）
//
// v3 修正：
//  1. 时序：8 命令位 + 1 空时钟(启动转换) + 12 数据位 = 21 时钟
//     （v2 只有 8+12=20，漏了启动转换的空时钟，读到的是无效残留）
//  2. 压力：改用 Z1+Z2 差分公式 Z = Z1 + 4096 - Z2（XPT2046 数据手册推荐）
//     比单 Z1 阈值更稳，不受按压力度影响
//  3. 阈值/窗口大幅放宽：Z_MIN=80、RAW 窗口 [100,4000]、采样偏差 150
//  4. 校准点内缩到 (50,50)/(270,190)，避开电阻屏边缘非线性区
//  5. 校准每点采样 4 次取平均
//  6. 串口持续输出原始值，方便诊断
#include <Arduino.h>
#include <Preferences.h>
#include "touch.h"

static Preferences tpPrefs;
static TouchCal tpCal;
static bool tpCalReady = false;

// ---------- 底层传输（修正时序：8 + 1 + 12 = 21 时钟）----------
// 沿间隔 3us ≈ 150kHz，低速稳定
static uint16_t tpXfer(uint8_t cmd) {
    uint16_t v = 0;
    digitalWrite(TP_CS, LOW);
    delayMicroseconds(1);

    // 8 个命令位（MSB first，上升沿采样 DIN）
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(TP_CLK, LOW);
        digitalWrite(TP_DIN, (cmd & 0x80) ? HIGH : LOW);
        cmd <<= 1;
        delayMicroseconds(3);
        digitalWrite(TP_CLK, HIGH);        // 上升沿：XPT2046 采样 DIN
        delayMicroseconds(3);
    }

    // 1 个空时钟：启动 ADC 转换（第 8 位后的这个时钟下降沿触发转换）
    digitalWrite(TP_CLK, LOW);
    digitalWrite(TP_DIN, LOW);
    delayMicroseconds(3);
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(10);                 // 等待 SAR 转换完成（max 10us）

    // 12 个数据位：下降沿后 DO 输出一位（MSB first）
    for (uint8_t i = 0; i < 12; i++) {
        digitalWrite(TP_CLK, LOW);
        delayMicroseconds(3);
        v = (v << 1) | (digitalRead(TP_DO) ? 1 : 0);
        digitalWrite(TP_CLK, HIGH);
        delayMicroseconds(3);
    }

    digitalWrite(TP_CS, HIGH);
    digitalWrite(TP_CLK, LOW);
    return v;
}

void tpInit() {
    pinMode(TP_CS, OUTPUT);
    pinMode(TP_CLK, OUTPUT);
    pinMode(TP_DIN, OUTPUT);
    pinMode(TP_DO, INPUT);                 // GPIO39 仅输入，无内部上下拉
    digitalWrite(TP_CS, HIGH);
    digitalWrite(TP_CLK, LOW);
    digitalWrite(TP_DIN, LOW);
    delayMicroseconds(100);
}

// 读一个通道，丢弃第一帧（首帧常有残留），返回第二帧
uint16_t tpReadChannel(uint8_t cmd) {
    tpXfer(cmd);
    delayMicroseconds(30);
    return tpXfer(cmd);
}

// 压力判断：用 Z1+Z2 差分公式（XPT2046 数据手册推荐）
//   Z = Z1 + 4096 - Z2，未按下时 Z ≈ 0，按下时 Z 随力度增大
bool tpPressed() {
    uint16_t z1 = tpXfer(TP_CMD_Z1);
    delayMicroseconds(30);
    uint16_t z2 = tpXfer(TP_CMD_Z1 + 0x10);   // 0xC0 = Z2 通道
    int32_t z = (int32_t)z1 + 4096 - (int32_t)z2;
    if (z < 0) z = 0;
    return z >= TP_Z_MIN;
}

// 串口调试输出原始值（仅在 TP_DEBUG 定义时启用）
void tpDebugDump() {
    uint16_t z1 = tpXfer(TP_CMD_Z1);
    delayMicroseconds(30);
    uint16_t z2 = tpXfer(0xC0);
    delayMicroseconds(30);
    uint16_t x = tpReadChannel(TP_CMD_X);
    uint16_t y = tpReadChannel(TP_CMD_Y);
    int32_t z = (int32_t)z1 + 4096 - (int32_t)z2;
    Serial.printf("[TP] Z1=%4u Z2=%4u Z=%4ld  X=%4u Y=%4u\n",
                  z1, z2, z, x, y);
}

bool tpReadRaw(uint16_t* rx, uint16_t* ry) {
    if (!tpPressed()) return false;
    // 一次性读 Z1→X→Y→Z1，验证压力持续
    uint16_t x1 = tpReadChannel(TP_CMD_X);
    uint16_t y1 = tpReadChannel(TP_CMD_Y);
    if (!tpPressed()) return false;
    uint16_t x2 = tpReadChannel(TP_CMD_X);
    uint16_t y2 = tpReadChannel(TP_CMD_Y);
    int dx = (int)x1 - (int)x2; if (dx < 0) dx = -dx;
    int dy = (int)y1 - (int)y2; if (dy < 0) dy = -dy;
    if (dx > TP_DEV || dy > TP_DEV) return false;       // 抖动过大
    uint16_t x = (x1 + x2) / 2, y = (y1 + y2) / 2;
    if (x < TP_RAW_MIN || x > TP_RAW_MAX) return false; // 超出合理窗口
    if (y < TP_RAW_MIN || y > TP_RAW_MAX) return false;
    *rx = x;
    *ry = y;
    return true;
}

// ---------- 校准 ----------
void tpSetCal(const TouchCal& cal) {
    tpCal = cal;
    tpCalReady = true;
}

bool tpLoadCal(TouchCal* cal) {
    tpPrefs.begin("touch", true);
    bool ok = tpPrefs.isKey("ax0");
    if (ok) {
        TouchCal c;
        c.ax0 = tpPrefs.getUShort("ax0");
        c.ax1 = tpPrefs.getUShort("ax1");
        c.ay0 = tpPrefs.getUShort("ay0");
        c.ay1 = tpPrefs.getUShort("ay1");
        c.swap = tpPrefs.getUChar("swap");
        tpSetCal(c);
        if (cal) *cal = c;
    }
    tpPrefs.end();
    return ok;
}

void tpStoreCal(const TouchCal& cal) {
    tpPrefs.begin("touch", false);
    tpPrefs.putUShort("ax0", cal.ax0);
    tpPrefs.putUShort("ax1", cal.ax1);
    tpPrefs.putUShort("ay0", cal.ay0);
    tpPrefs.putUShort("ay1", cal.ay1);
    tpPrefs.putUChar("swap", cal.swap);
    tpPrefs.end();
    tpSetCal(cal);
}

bool tpMap(uint16_t rx, uint16_t ry, uint16_t* sx, uint16_t* sy) {
    if (!tpCalReady) return false;
    uint16_t ra = tpCal.swap ? ry : rx;   // 屏幕 X 轴对应的原始通道
    uint16_t rb = tpCal.swap ? rx : ry;
    int32_t dxa = (int32_t)tpCal.ax1 - (int32_t)tpCal.ax0;
    int32_t dya = (int32_t)tpCal.ay1 - (int32_t)tpCal.ay0;
    if (dxa == 0 || dya == 0) return false;
    int32_t x = TP_CAL_X0 + ((int32_t)ra - tpCal.ax0) * (TP_CAL_X1 - TP_CAL_X0) / dxa;
    int32_t y = TP_CAL_Y0 + ((int32_t)rb - tpCal.ay0) * (TP_CAL_Y1 - TP_CAL_Y0) / dya;
    if (x < 0) x = 0; if (x > 319) x = 319;
    if (y < 0) y = 0; if (y > 239) y = 239;
    *sx = (uint16_t)x;
    *sy = (uint16_t)y;
    return true;
}

bool tpReadScreen(uint16_t* sx, uint16_t* sy) {
    uint16_t rx, ry;
    if (!tpReadRaw(&rx, &ry)) return false;
    return tpMap(rx, ry, sx, sy);
}

// ---------- 校准采样（阻塞式）----------
// 采样 4 次取平均，更稳
bool tpWaitStableRaw(uint16_t* rx, uint16_t* ry, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    // 先等待释放
    while (millis() - t0 < timeoutMs) {
        if (!tpPressed()) break;
        delay(10);
    }
    // 等待按下
    while (millis() - t0 < timeoutMs) {
        if (tpPressed()) {
            delay(80);   // 等按稳
            // 采集 4 次有效样本取平均
            uint32_t sx = 0, sy = 0;
            uint8_t n = 0;
            for (uint8_t i = 0; i < 8 && n < 4; i++) {
                uint16_t x, y;
                if (tpReadRaw(&x, &y)) {
                    sx += x; sy += y; n++;
                }
                delay(20);
            }
            if (n >= 2) {
                *rx = sx / n;
                *ry = sy / n;
                return true;
            }
        }
        delay(10);
    }
    return false;
}
