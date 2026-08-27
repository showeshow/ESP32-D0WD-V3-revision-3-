// ============================================================
//  ESP32-2432S028 (CYD) 学生端答题器 v2
//  横屏 320x240 ILI9341 + XPT2046 电阻触摸（bit-bang 自研驱动）
//
//  v2 变更：
//   - 触摸：弃用 TFT_eSPI getTouch()（它只走共享 SPI 总线，CYD 的
//     XPT2046 在独立引脚上，读不到）。改为 src/touch.cpp 的 bit-bang
//     软 SPI 驱动：纯轮询、无 IRQ、时钟 ~200kHz、双采样过滤、
//     两点触摸校准（首次开机自动进入，校准值存 Preferences）
//   - 登录界面：去掉网络状态/网址显示，数字键与确认键放大占满屏幕；
//     右上角透明齿轮图标进入设置
//   - 设置界面：SSID/密码输入 + QWERTY 键盘 + 保存 / 触摸校准
//
//  性能策略：
//   - 触摸纯轮询（无 attachInterrupt），20ms 节流
//   - 禁用 I2S / 音频 / 喇叭 / SD 卡 / 蓝牙 / OTA
//   - loop 节流：触摸 50Hz、WiFi 检查 1Hz、仅状态变化时重绘
//   - 无 LVGL；中文界面为自生成 24x24 点阵字库
// ============================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "config.h"
#include "touch.h"

extern const char ZH_INDEX[];
extern const uint8_t ZH_FONT[];

TFT_eSPI tft;
Preferences prefs;

// ---------------- 界面状态 ----------------
enum Screen : uint8_t { SCR_LOGIN, SCR_ANSWER, SCR_SETTING };
Screen screen = SCR_LOGIN;

String ipField[3];            // 登录界面：IP段3 / IP段4 / 端口
uint8_t activeField = 0;
char serverIP[24] = "";

char answers[ANS_COUNT];      // 0=未答, 'A'..'D', 'T'=对(勾), 'F'=错(叉)
uint8_t activeCell = 0;
uint32_t submitCount = 0;
int lastHttp = 0;

// WiFi（运行时生效值，可在设置界面改）
char wifiSsid[SET_TEXT_MAX + 1] = WIFI_SSID;
char wifiPass[SET_TEXT_MAX + 1] = WIFI_PASSWORD;

// 设置界面状态
char ssidBuf[SET_TEXT_MAX + 1];
char passBuf[SET_TEXT_MAX + 1];
uint8_t setTarget = 0;        // 0=SSID 框 1=密码框
uint8_t kbMode = 0;           // 0=小写 1=大写 2=数字

// ---------------- 触摸状态（纯轮询）----------------
bool touching = false;
uint32_t tLastRelease = 0;
uint32_t tTouchPoll = 0;
uint32_t tWifiTick = 0;
uint32_t tReconnect = 0;
bool wifiLogged = false;

// ---------------- 临时状态消息 ----------------
char statusMsg[48] = "";
uint16_t statusColor = C_TEXT;
uint32_t statusUntil = 0;

// ---------------- 前向声明 ----------------
static void refreshStatus();
void loadPrefs();
void savePrefs();

// ---------------- 布局 ----------------
struct Rect { int16_t x, y, w, h; };

// 登录界面（键盘放大占满，右上角齿轮）
static const Rect fieldR[3] = {
    {52, 8, 58, 38},     // IP 段3
    {131, 8, 58, 38},    // IP 段4
    {210, 8, 58, 38},    // 端口
};
static const Rect numR[10] = {
    {6, 54, 42, 86}, {52, 54, 42, 86}, {98, 54, 42, 86}, {144, 54, 42, 86}, {190, 54, 42, 86},
    {6, 146, 42, 86}, {52, 146, 42, 86}, {98, 146, 42, 86}, {144, 146, 42, 86}, {190, 146, 42, 86},
};
static const Rect confirmR = {238, 54, 76, 178};
static const Rect gearR = {284, 4, 32, 32};      // 右上角齿轮点击区

// 答题界面
static Rect cellR[ANS_COUNT];
static const Rect ansKeyR[6] = {
    {10, 62, 58, 46}, {73, 62, 58, 46}, {136, 62, 58, 46},
    {10, 113, 58, 46}, {73, 113, 58, 46}, {136, 113, 58, 46},
};
static const Rect sendR = {202, 62, 110, 97};
static const Rect backR = {266, 200, 50, 36};      // 答题界面右下角返回键

// 设置界面：5 行 × 10 列键盘网格（含功能键占位）
// 屏幕宽 320，每格 32 宽；行高 33，5 行从 y=60 到 y=233
static const Rect ssidBoxR = {4, 4, 312, 24};     // name 框（缩短）
static const Rect passBoxR = {4, 32, 312, 24};    // code 框（缩短）
#define KB_X0    0
#define KB_Y0    60
#define KB_CELL_W 32
#define KB_CELL_H 33
#define KB_GAP    0
#define KB_COLS  10
#define KB_ROWS  5
#define KB_COUNT (KB_ROWS * KB_COLS)              // 50 个格子

// 按键 ID
enum : uint8_t {
    BTN_NONE = 0xFF,
    BTN_NUM0 = 0, BTN_NUM1, BTN_NUM2, BTN_NUM3, BTN_NUM4,
    BTN_NUM5, BTN_NUM6, BTN_NUM7, BTN_NUM8, BTN_NUM9,
    BTN_CONFIRM,
    BTN_FIELD0, BTN_FIELD1, BTN_FIELD2,
    BTN_CELL0, BTN_CELL1, BTN_CELL2, BTN_CELL3, BTN_CELL4,
    BTN_CELL5, BTN_CELL6, BTN_CELL7, BTN_CELL8, BTN_CELL9,
    BTN_ANS_A, BTN_ANS_B, BTN_ANS_C, BTN_ANS_D, BTN_ANS_T, BTN_ANS_F,
    BTN_SEND,
    BTN_BACK,                       // 答题界面右下角返回键
    BTN_GEAR,
    BTN_SET_SSID, BTN_SET_PASS,
    BTN_KB0,                       // 50 个键盘格子（含功能键位）
    BTN_KB_LAST = BTN_KB0 + KB_COUNT - 1,
};
// 键盘格子中的功能键位（在 5x10 网格中的固定位置）
// 行5(idx 40-49): 40=大小写 41=空格 42-47=,.!?*+ 48=退格 49=保存(保存即返回登录)
#define KB_IDX_SHIFT   40
#define KB_IDX_SPACE   41
#define KB_IDX_BACK    48
#define KB_IDX_SAVE    49
static uint8_t activeBtn = BTN_NONE;

// ============================================================
//  中文点阵渲染（24x24）
// ============================================================
static int zhLookup(const char* p) {
    const char* q = ZH_INDEX;
    while (*q) {
        if (q[0] == p[0] && q[1] == p[1] && q[2] == p[2]) return (int)((q - ZH_INDEX) / 3);
        q += 3;
    }
    return -1;
}

static uint16_t zhBuf[24 * 24];

static int16_t drawMix(int16_t x, int16_t y, const char* s, uint16_t fg, uint16_t bg) {
    const char* p = s;
    while (*p) {
        uint8_t c = (uint8_t)*p;
        if (c >= 0xE0 && p[1] && p[2]) {
            int idx = zhLookup(p);
            if (idx >= 0) {
                const uint8_t* g = ZH_FONT + (size_t)idx * 72;
                for (int r = 0; r < 24; r++)
                    for (int cc = 0; cc < 24; cc++)
                        zhBuf[r * 24 + cc] = (g[r * 3 + (cc >> 3)] & (0x80 >> (cc & 7))) ? fg : bg;
                tft.pushImage(x, y, 24, 24, zhBuf);
            } else {
                tft.fillRect(x, y, 24, 24, bg);
            }
            x += 24; p += 3;
        } else if (c == ' ') {
            tft.fillRect(x, y, 12, 24, bg);
            x += 12; p++;
        } else if (c >= 32 && c < 127) {
            tft.setTextDatum(TL_DATUM);
            tft.setTextFont(2);
            tft.setTextColor(fg, bg);
            tft.drawChar((char)c, x, y + 4);
            x += 12; p++;
        } else {
            p++;
        }
    }
    return x;
}

static int16_t measureMix(const char* s) {
    int16_t w = 0;
    for (const char* p = s; *p; ) {
        if ((uint8_t)*p >= 0xE0 && p[1] && p[2]) { w += 24; p += 3; }
        else { w += 12; p++; }
    }
    return w;
}

static void drawMixCenter(int16_t cx, int16_t y, const char* s, uint16_t fg, uint16_t bg) {
    drawMix(cx - measureMix(s) / 2, y, s, fg, bg);
}

// ============================================================
//  图形：勾 / 叉 / 齿轮 / 退格箭头
// ============================================================
static void drawCheck(int16_t x, int16_t y, int16_t s, uint16_t c) {
    int16_t x1 = x + s * 3 / 20,  y1 = y + s * 11 / 20;
    int16_t x2 = x + s * 8 / 20,  y2 = y + s * 16 / 20;
    int16_t x3 = x + s * 17 / 20, y3 = y + s * 4 / 20;
    tft.drawLine(x1, y1, x2, y2, c);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, c);
    tft.drawLine(x1, y1 + 1, x2, y2 + 1, c);
    tft.drawLine(x2, y2, x3, y3, c);
    tft.drawLine(x2 + 1, y2, x3 + 1, y3, c);
    tft.drawLine(x2, y2 + 1, x3, y3 + 1, c);
}

static void drawCross(int16_t x, int16_t y, int16_t s, uint16_t c) {
    int16_t m = s / 5;
    tft.drawLine(x + m, y + m, x + s - m, y + s - m, c);
    tft.drawLine(x + m + 1, y + m, x + s - m + 1, y + s - m, c);
    tft.drawLine(x + m, y + m + 1, x + s - m, y + s - m + 1, c);
    tft.drawLine(x + s - m, y + m, x + m, y + s - m, c);
    tft.drawLine(x + s - m - 1, y + m, x + m - 1, y + s - m, c);
    tft.drawLine(x + s - m, y + m + 1, x + m, y + s - m + 1, c);
}

// 透明齿轮（直接画在背景上，无底块）
static void drawGear(int16_t cx, int16_t cy, uint16_t c) {
    tft.drawCircle(cx, cy, 10, c);
    tft.drawCircle(cx, cy, 5, c);
    for (uint8_t a = 0; a < 8; a++) {
        float rad = a * 0.7853982f;
        int16_t x1 = cx + (int16_t)(cosf(rad) * 8.5f);
        int16_t y1 = cy + (int16_t)(sinf(rad) * 8.5f);
        int16_t x2 = cx + (int16_t)(cosf(rad) * 13.5f);
        int16_t y2 = cy + (int16_t)(sinf(rad) * 13.5f);
        tft.drawLine(x1, y1, x2, y2, c);
        tft.drawLine(x1 + 1, y1, x2 + 1, y2, c);
    }
}

static void drawBackspace(int16_t cx, int16_t cy, int16_t s, uint16_t c) {
    // 左箭头（键盘删除键）
    int16_t hw = s / 2;
    tft.drawLine(cx + 2, cy - hw, cx - hw, cy, c);
    tft.drawLine(cx + 2, cy - hw + 1, cx - hw + 1, cy, c);
    tft.drawLine(cx + 2, cy + hw, cx - hw, cy, c);
    tft.drawLine(cx + 2, cy + hw - 1, cx - hw + 1, cy, c);
    tft.drawLine(cx - hw, cy, cx + hw, cy, c);
    tft.drawLine(cx - hw, cy - 1, cx + hw, cy - 1, c);
    tft.drawLine(cx - hw, cy + 1, cx + hw, cy + 1, c);
}

// ============================================================
//  登录界面绘制
// ============================================================
static void drawIpField(uint8_t i) {
    const Rect& r = fieldR[i];
    uint16_t border = (i == activeField) ? C_FRAME_HI : C_FRAME;
    tft.fillRect(r.x, r.y, r.w, r.h, C_PANEL);
    tft.drawRect(r.x, r.y, r.w, r.h, border);
    if (i == activeField) tft.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, border);
    const String& s = ipField[i];
    if (s.length()) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_TEXT, C_PANEL);
        tft.drawString(s, r.x + r.w / 2, r.y + r.h / 2 - 3);
    }
    if (i == activeField) {
        tft.fillRect(r.x + 4, r.y + r.h - 7, r.w - 8, 3, C_FRAME_HI);
    }
}

static void drawNumKey(uint8_t d, bool pressed) {
    const Rect& r = numR[d];
    uint16_t bg = pressed ? 0x5D1F : C_KEY_BG;
    tft.fillRect(r.x, r.y, r.w, r.h, bg);
    tft.drawRect(r.x, r.y, r.w, r.h, C_FRAME);
    char buf[2] = {(char)('0' + d), 0};
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_KEY_TEXT, bg);
    tft.drawString(buf, r.x + r.w / 2, r.y + r.h / 2);
}

static void drawConfirmKey(bool pressed) {
    uint16_t bg = pressed ? 0x5D1F : C_KEY_BG2;
    tft.fillRect(confirmR.x, confirmR.y, confirmR.w, confirmR.h, bg);
    tft.drawRect(confirmR.x, confirmR.y, confirmR.w, confirmR.h, C_FRAME);
    drawMix(confirmR.x + (confirmR.w - 48) / 2, confirmR.y + (confirmR.h - 24) / 2,
            "确认", C_KEY_TEXT, bg);
}

static void drawLoginScreen() {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT_DIM, C_BG);
    tft.drawString(".", 120, 27);
    tft.drawString(":", 199, 27);
    for (uint8_t i = 0; i < 3; i++) drawIpField(i);
    for (uint8_t d = 0; d < 10; d++) drawNumKey(d, false);
    drawConfirmKey(false);
    drawGear(gearR.x + gearR.w / 2, gearR.y + gearR.h / 2, C_TEXT_DIM);
}

// 登录界面临时消息横幅（覆盖键盘上部，到期全屏重绘）
static void drawLoginBanner() {
    tft.fillRect(0, 90, 320, 42, C_PANEL);
    tft.drawRect(0, 90, 320, 42, statusColor);
    drawMixCenter(160, 99, statusMsg, statusColor, C_PANEL);
}

static void showStatus(const char* msg, uint16_t color) {
    strncpy(statusMsg, msg, sizeof(statusMsg) - 1);
    statusMsg[sizeof(statusMsg) - 1] = 0;
    statusColor = color;
    statusUntil = millis() + 2000;
    if (screen == SCR_ANSWER) refreshStatus();
    else if (screen == SCR_LOGIN) drawLoginBanner();
}

// ============================================================
//  答题界面绘制
// ============================================================
static void wifiText(char* buf, size_t n) {
    if (WiFi.status() == WL_CONNECTED) snprintf(buf, n, "网络:已连接");
    else snprintf(buf, n, "网络:未连接");
}

static void refreshStatus() {
    if (screen != SCR_ANSWER) return;
    tft.fillRect(0, 167, 320, 73, C_BG);
    tft.drawFastHLine(0, 166, 320, C_FRAME);
    int16_t x = 10;
    if (statusUntil && millis() < statusUntil) {
        drawMix(x, 172, statusMsg, statusColor, C_BG);
    } else {
        char n[12];
        snprintf(n, sizeof(n), "%lu", (unsigned long)submitCount);
        x = drawMix(x, 172, "已提交:", C_TEXT, C_BG);
        x = drawMix(x, 172, n, C_ANS, C_BG);
        x = drawMix(x + 4, 172, "次", C_TEXT, C_BG);
        if (lastHttp >= 200 && lastHttp < 300) {
            char r[20];
            snprintf(r, sizeof(r), "(%d)", lastHttp);
            x = drawMix(x + 6, 172, "上次:成功", C_OK, C_BG);
            drawMix(x, 172, r, C_OK, C_BG);
        } else if (lastHttp != 0) {
            drawMix(x + 6, 172, "上次:失败", C_ERR, C_BG);
        }
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT_DIM, C_BG);
    // serverIP 限制在返回键左侧（x < 260），避免重叠
    String ipStr = serverIP;
    if (ipStr.length() > 22) ipStr = ipStr.substring(0, 22) + "..";
    tft.drawString(ipStr, 10, 206);
}

static void drawCell(uint8_t i) {
    const Rect& r = cellR[i];
    uint16_t border = (i == activeCell) ? C_FRAME_HI : C_FRAME;
    tft.fillRect(r.x, r.y, r.w, r.h, C_PANEL);
    tft.drawRect(r.x, r.y, r.w, r.h, border);
    char nb[4];
    snprintf(nb, sizeof(nb), "%u", i + 1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(C_TEXT_DIM, C_PANEL);
    tft.drawString(nb, r.x + r.w / 2, r.y + r.h - 7);
    char a = answers[i];
    if (a == 'T') {
        drawCheck(r.x + (r.w - 18) / 2, r.y + 6, 18, C_ANS);
    } else if (a == 'F') {
        drawCross(r.x + (r.w - 18) / 2, r.y + 6, 18, C_ANS);
    } else if (a) {
        char ab[2] = {a, 0};
        tft.setTextFont(2);
        tft.setTextColor(C_ANS, C_PANEL);
        tft.drawString(ab, r.x + r.w / 2, r.y + 13);
    }
    if (i == activeCell) {
        tft.fillRect(r.x + 4, r.y + r.h - 16, r.w - 8, 3, C_FRAME_HI);
    }
}

static void drawAnsKey(uint8_t k, bool pressed) {
    const Rect& r = ansKeyR[k];
    uint16_t bg = pressed ? 0x5D1F : C_KEY_BG;
    tft.fillRect(r.x, r.y, r.w, r.h, bg);
    tft.drawRect(r.x, r.y, r.w, r.h, C_FRAME);
    if (k == 4) {
        drawCheck(r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2, 24, C_KEY_TEXT);
    } else if (k == 5) {
        drawCross(r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2, 24, C_KEY_TEXT);
    } else {
        char ab[2] = {(char)('A' + k), 0};
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(C_KEY_TEXT, bg);
        tft.drawString(ab, r.x + r.w / 2, r.y + r.h / 2);
    }
}

static void drawSendKey(bool pressed) {
    uint16_t bg = pressed ? 0x5D1F : C_KEY_BG2;
    tft.fillRect(sendR.x, sendR.y, sendR.w, sendR.h, bg);
    tft.drawRect(sendR.x, sendR.y, sendR.w, sendR.h, C_FRAME);
    drawMix(sendR.x + (sendR.w - 48) / 2, sendR.y + (sendR.h - 24) / 2,
            "发送", C_KEY_TEXT, bg);
}

static void drawBackKey(bool pressed) {
    uint16_t bg = pressed ? 0x5D1F : C_KEY_BG;
    tft.fillRect(backR.x, backR.y, backR.w, backR.h, bg);
    tft.drawRect(backR.x, backR.y, backR.w, backR.h, C_FRAME);
    drawMix(backR.x + (backR.w - 24) / 2, backR.y + (backR.h - 24) / 2,
            "返回", C_KEY_TEXT, bg);
}

static void drawAnswerScreen() {
    tft.fillScreen(C_BG);
    for (uint8_t i = 0; i < ANS_COUNT; i++) drawCell(i);
    for (uint8_t k = 0; k < 6; k++) drawAnsKey(k, false);
    drawSendKey(false);
    drawBackKey(false);
    refreshStatus();
}

// ============================================================
//  设置界面（WiFi）绘制 — 5 行 × 10 列键盘网格
// ============================================================
// 5 行内容（每行 10 格）：
//   行1: 1 2 3 4 5 6 7 8 9 0
//   行2: q w e r t y u i o p
//   行3: a s d f g h j k l @ .
//   行4: z x c v b n m - _ % #
//   行5: * , / ! ? (空格占6格) (大小写) (退格) (保存)
// 大小写模式切换时行2-4 字母大小写；符号行不变
static const char KB_LOWER[3][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','@'},
    {'z','x','c','v','b','n','m','-','_','%'},
};
static const char KB_UPPER[3][10] = {
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L','@'},
    {'Z','X','C','V','B','N','M','-','_','%'},
};
static const char KB_ROW1[10] = {'1','2','3','4','5','6','7','8','9','0'};
// 行5 符号（idx 42-47）: , . ! ? * +
static const char KB_ROW5_SYM[6] = {',', '.', '!', '?', '*', '+'};

// 取键盘格子 idx(0..49) 对应的字符，0=功能键
static char kbCharAt(uint8_t idx) {
    uint8_t row = idx / 10;
    uint8_t col = idx % 10;
    if (row == 0) return KB_ROW1[col];
    if (row >= 1 && row <= 3) {
        return (kbMode == 1) ? KB_UPPER[row - 1][col] : KB_LOWER[row - 1][col];
    }
    // row 4 (idx 40-49)：大小写(40) 空格(41) , . ! ? * +(42-47) 退格(48) 保存(49)
    if (col == 40 - 40) return 0;            // 大小写
    if (col == 41 - 40) return 0;            // 空格
    if (col >= 42 - 40 && col <= 47 - 40) return KB_ROW5_SYM[col - 2];
    if (col == 48 - 40) return 0;            // 退格
    if (col == 49 - 40) return 0;            // 保存
    return 0;
}

static Rect kbCellRect(uint8_t idx) {
    uint8_t row = idx / 10, col = idx % 10;
    return {int16_t(KB_X0 + col * (KB_CELL_W + KB_GAP)),
            int16_t(KB_Y0 + row * (KB_CELL_H + KB_GAP)),
            KB_CELL_W, KB_CELL_H};
}

static void drawSetBox(uint8_t target) {
    const Rect& r = (target == 0) ? ssidBoxR : passBoxR;
    const char* label = (target == 0) ? "name:" : "code:";
    const char* text = (target == 0) ? ssidBuf : passBuf;
    uint16_t border = (setTarget == target) ? C_FRAME_HI : C_FRAME;
    tft.fillRect(r.x, r.y, r.w, r.h, C_PANEL);
    tft.drawRect(r.x, r.y, r.w, r.h, border);
    if (setTarget == target) tft.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, border);
    // label 用 ASCII（name:/code:）
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT_DIM, C_PANEL);
    tft.drawString(label, r.x + 6, r.y + 4);
    int16_t lx = r.x + 6 + 6 * 5;   // "name:" 5 字符 × 6px = 30，留 6px
    // 内容（ASCII，截断到框宽）
    tft.setTextColor(C_TEXT, C_PANEL);
    uint8_t maxCh = (r.w - (lx - r.x) - 8) / 7;
    size_t len = strlen(text);
    if (len > maxCh) {
        char tmp[SET_TEXT_MAX + 1];
        strncpy(tmp, text + (len - maxCh), maxCh);
        tmp[maxCh] = 0;
        tft.drawString(tmp, lx, r.y + 4);
    } else {
        tft.drawString(text, lx, r.y + 4);
    }
    if (setTarget == target) {
        tft.fillRect(r.x + 4, r.y + r.h - 4, r.w - 8, 2, C_FRAME_HI);
    }
}

static void drawKbCell(uint8_t idx, bool pressed) {
    Rect r = kbCellRect(idx);
    bool isSave = (idx == KB_IDX_SAVE);
    bool isShift = (idx == KB_IDX_SHIFT);
    bool isSpace = (idx == KB_IDX_SPACE);
    bool isBack = (idx == KB_IDX_BACK);

    uint16_t bg;
    if (isSave) bg = pressed ? 0x5D1F : C_KEY_BG2;
    else if (isShift && kbMode == 1) bg = pressed ? 0x5D1F : 0x2A69;
    else bg = pressed ? 0x5D1F : C_KEY_BG;
    tft.fillRect(r.x, r.y, r.w, r.h, bg);
    tft.drawRect(r.x, r.y, r.w, r.h, C_FRAME);

    if (isShift) {
        // 32px 格子放不下"大小写"3字，用 Aa 图标
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_KEY_TEXT, bg);
        tft.drawString(kbMode == 1 ? "AA" : "aa", r.x + r.w / 2, r.y + r.h / 2);
        return;
    }
    if (isSpace) {
        // 32px 放"空格"2字=48px 也超，用 _ 下划线符号代替
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_KEY_TEXT, bg);
        tft.drawString("_", r.x + r.w / 2, r.y + r.h / 2);
        return;
    }
    if (isBack) {
        drawBackspace(r.x + r.w / 2, r.y + r.h / 2, 18, C_KEY_TEXT);
        return;
    }
    if (isSave) {
        // 32px 放"保存"2字=48px 超，用 OK 代替
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_KEY_TEXT, bg);
        tft.drawString("OK", r.x + r.w / 2, r.y + r.h / 2);
        return;
    }
    char c = kbCharAt(idx);
    if (c) {
        char buf[2] = {c, 0};
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_KEY_TEXT, bg);
        tft.drawString(buf, r.x + r.w / 2, r.y + r.h / 2);
    }
}

static void drawSettingScreen() {
    tft.fillScreen(C_BG);
    drawSetBox(0);
    drawSetBox(1);
    for (uint8_t i = 0; i < KB_COUNT; i++) drawKbCell(i, false);
}

// ============================================================
//  触摸校准（阻塞式流程）
// ============================================================
static void drawCalibPoint(int16_t x, int16_t y, uint16_t c) {
    tft.drawCircle(x, y, 10, c);
    tft.fillCircle(x, y, 3, c);
    tft.drawLine(x - 16, y, x - 6, y, c);
    tft.drawLine(x + 6, y, x + 16, y, c);
    tft.drawLine(x, y - 16, x, y - 6, c);
    tft.drawLine(x, y + 6, x, y + 16, c);
}

void runCalibration() {
    uint16_t x1r, y1r, x2r, y2r;
    Serial.println(F("[CAL] 校准开始"));

    // 第一点：左上（内缩 50px，避开电阻屏边缘非线性区）
    tft.fillScreen(C_BG);
    drawCalibPoint(TP_CAL_X0, TP_CAL_Y0, C_FRAME_HI);
    drawMixCenter(160, 90, "触摸校准", C_TEXT, C_BG);
    drawMixCenter(160, 124, "点击亮色圆点", C_ANS, C_BG);
    drawMixCenter(160, 158, "(左上)", C_TEXT_DIM, C_BG);
    if (!tpWaitStableRaw(&x1r, &y1r, 30000)) {
        Serial.println(F("[CAL] 第一点超时"));
        return;
    }
    tft.fillCircle(TP_CAL_X0, TP_CAL_Y0, 5, C_OK);
    Serial.printf("[CAL] p1 raw=%u,%u\n", x1r, y1r);
    while (tpPressed()) delay(20);
    delay(500);

    // 第二点：右下（内缩 50px）
    tft.fillScreen(C_BG);
    drawCalibPoint(TP_CAL_X1, TP_CAL_Y1, C_FRAME_HI);
    drawMixCenter(160, 90, "触摸校准", C_TEXT, C_BG);
    drawMixCenter(160, 124, "点击亮色圆点", C_ANS, C_BG);
    drawMixCenter(160, 158, "(右下)", C_TEXT_DIM, C_BG);
    if (!tpWaitStableRaw(&x2r, &y2r, 30000)) {
        Serial.println(F("[CAL] 第二点超时"));
        return;
    }
    tft.fillCircle(TP_CAL_X1, TP_CAL_Y1, 5, C_OK);
    Serial.printf("[CAL] p2 raw=%u,%u\n", x2r, y2r);

    // 计算映射（比较两轴 raw 变化幅度判定 swap）
    TouchCal cal;
    int32_t dx = (int32_t)x2r - (int32_t)x1r;
    int32_t dy = (int32_t)y2r - (int32_t)y1r;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx >= dy) {
        cal.swap = 0;
        cal.ax0 = x1r; cal.ax1 = x2r;
        cal.ay0 = y1r; cal.ay1 = y2r;
    } else {
        cal.swap = 1;
        cal.ax0 = y1r; cal.ax1 = y2r;
        cal.ay0 = x1r; cal.ay1 = x2r;
    }
    tpStoreCal(cal);
    Serial.printf("[CAL] 完成 swap=%u ax=[%u,%u] ay=[%u,%u]\n",
                  cal.swap, cal.ax0, cal.ax1, cal.ay0, cal.ay1);
    tft.fillScreen(C_BG);
    drawMixCenter(160, 108, "校准完成", C_OK, C_BG);
    delay(900);
}

// ============================================================
//  交互逻辑
// ============================================================
static void inputDigit(uint8_t d) {
    String& s = ipField[activeField];
    uint8_t maxLen = (activeField == 2) ? 4 : 3;
    if (s.length() >= maxLen) {
        if (activeField < 2 && ipField[activeField + 1].length() < ((activeField + 1 == 2) ? 4 : 3)) {
            uint8_t old = activeField;
            activeField++;
            drawIpField(old);
            inputDigit(d);
        }
        return;
    }
    s += (char)('0' + d);
    if (s.length() >= maxLen && activeField < 2) {
        uint8_t old = activeField;
        activeField++;
        drawIpField(old);
    }
    drawIpField(activeField);
}

static void tapField(uint8_t i) {
    uint8_t old = activeField;
    activeField = i;
    ipField[i] = "";
    if (old != i) drawIpField(old);
    drawIpField(i);
}

static void onConfirm() {
    if (!ipField[0].length() || !ipField[1].length() || !ipField[2].length()) {
        showStatus("请输入完整地址", C_ERR);
        return;
    }
    long a = ipField[0].toInt(), b = ipField[1].toInt(), p = ipField[2].toInt();
    if (a < 0 || a > 255 || b < 0 || b > 255 || p <= 0 || p > 65535) {
        showStatus("地址无效", C_ERR);
        return;
    }
    snprintf(serverIP, sizeof(serverIP), "192.168.%s.%s:%s",
             ipField[0].c_str(), ipField[1].c_str(), ipField[2].c_str());
    savePrefs();
    screen = SCR_ANSWER;
    activeCell = 0;
    Serial.printf("[APP] server=%s count=%lu\n", serverIP, (unsigned long)submitCount);
    drawAnswerScreen();
}

static void inputAnswer(char a) {
    uint8_t old = activeCell;
    answers[old] = a;
    drawCell(old);
    if (activeCell < ANS_COUNT - 1) {
        activeCell++;
        drawCell(activeCell);
    }
}

static void tapCell(uint8_t i) {
    uint8_t old = activeCell;
    activeCell = i;
    drawCell(old);
    drawCell(i);
}

// 提交单个答案到 ClassroomReceiver
// 接口: POST /api/answer  Content-Type: application/json  body: {"choice":"A"}
// 答案映射: A/B/C/D 不变, T->"对", F->"错"
static int postAnswer(uint8_t cellIdx) {
    char a = answers[cellIdx];
    if (a == 0) return -1;   // 该格未答
    const char* choice;
    if (a == 'T') choice = "\xE5\xAF\xB9";        // "对" UTF-8
    else if (a == 'F') choice = "\xE9\x94\x99";   // "错" UTF-8
    else choice = "ABCD";                          // A/B/C/D 用下面取
    char choiceStr[4];
    if (a == 'T' || a == 'F') {
        strcpy(choiceStr, choice);
    } else {
        choiceStr[0] = a; choiceStr[1] = 0;
    }
    WiFiClient client;
    HTTPClient http;
    String url = "http://";
    url += serverIP;
    url += "/api/answer";
    if (!http.begin(client, url)) return -100;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    String body = "{\"choice\":\"";
    body += choiceStr;
    body += "\"}";
    Serial.printf("[HTTP] POST %s\n", url.c_str());
    Serial.printf("[HTTP] body %s (cell %u)\n", body.c_str(), cellIdx + 1);
    int code = http.POST(body);
    String resp = http.getString();
    Serial.printf("[HTTP] -> %d resp=%s\n", code, resp.c_str());
    http.end();
    return code;
}

static void onSend() {
    if (WiFi.status() != WL_CONNECTED) {
        showStatus("网络未连接", C_ERR);
        return;
    }
    // 检查当前格子是否有答案
    if (answers[activeCell] == 0) {
        showStatus("请先选择答案", C_ERR);
        return;
    }
    submitCount++;
    savePrefs();
    showStatus("正在发送...", C_ANS);
    lastHttp = postAnswer(activeCell);
    if (lastHttp >= 200 && lastHttp < 300) {
        char msg[24];
        snprintf(msg, sizeof(msg), "Q%u已提交", activeCell + 1);
        showStatus(msg, C_OK);
        Serial.printf("[HTTP] OK seq=%lu\n", (unsigned long)submitCount);
    } else if (lastHttp < 0) {
        showStatus("未响应", C_ERR);
    } else {
        showStatus("发送失败", C_ERR);
        Serial.printf("[HTTP] error code=%d\n", lastHttp);
    }
    // 提交后：清空所有格子 + 光标回到第 1 格
    activeCell = 0;
    memset(answers, 0, sizeof(answers));
    for (uint8_t i = 0; i < ANS_COUNT; i++) drawCell(i);
}

// ---------- 设置界面交互 ----------
static void inputChar(char c) {
    char* buf = (setTarget == 0) ? ssidBuf : passBuf;
    size_t len = strlen(buf);
    if (len < SET_TEXT_MAX) {
        buf[len] = c;
        buf[len + 1] = 0;
        drawSetBox(setTarget);
    }
}

static void inputBackspace() {
    char* buf = (setTarget == 0) ? ssidBuf : passBuf;
    size_t len = strlen(buf);
    if (len > 0) {
        buf[len - 1] = 0;
        drawSetBox(setTarget);
    }
}

static void toggleShift() {
    kbMode = (kbMode == 1) ? 0 : 1;
    // 重绘字母行（行 1-3，即 idx 10-39）
    for (uint8_t i = 10; i < 40; i++) drawKbCell(i, false);
    drawKbCell(KB_IDX_SHIFT, false);
}

static void saveWifi() {
    strncpy(wifiSsid, ssidBuf, SET_TEXT_MAX);
    wifiSsid[SET_TEXT_MAX] = 0;
    strncpy(wifiPass, passBuf, SET_TEXT_MAX);
    wifiPass[SET_TEXT_MAX] = 0;
    prefs.begin("responder", false);
    prefs.putString("wsid", wifiSsid);
    prefs.putString("wpas", wifiPass);
    prefs.end();
    WiFi.disconnect();
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("[WIFI] new creds ssid=%s\n", wifiSsid);
    screen = SCR_LOGIN;
    drawLoginScreen();
}

static void enterSetting() {
    strncpy(ssidBuf, wifiSsid, SET_TEXT_MAX);
    ssidBuf[SET_TEXT_MAX] = 0;
    strncpy(passBuf, wifiPass, SET_TEXT_MAX);
    passBuf[SET_TEXT_MAX] = 0;
    setTarget = 0;
    kbMode = 0;
    screen = SCR_SETTING;
    drawSettingScreen();
}

// ============================================================
//  触摸：纯轮询（无 IRQ / 无中断）
// ============================================================
static bool inRect(int16_t x, int16_t y, const Rect& r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static uint8_t hitTest(int16_t x, int16_t y) {
    if (screen == SCR_LOGIN) {
        if (inRect(x, y, gearR)) return BTN_GEAR;
        for (uint8_t i = 0; i < 3; i++) if (inRect(x, y, fieldR[i])) return BTN_FIELD0 + i;
        for (uint8_t d = 0; d < 10; d++) if (inRect(x, y, numR[d])) return BTN_NUM0 + d;
        if (inRect(x, y, confirmR)) return BTN_CONFIRM;
    } else if (screen == SCR_ANSWER) {
        for (uint8_t i = 0; i < ANS_COUNT; i++) if (inRect(x, y, cellR[i])) return BTN_CELL0 + i;
        for (uint8_t k = 0; k < 6; k++) if (inRect(x, y, ansKeyR[k])) return BTN_ANS_A + k;
        if (inRect(x, y, sendR)) return BTN_SEND;
        if (inRect(x, y, backR)) return BTN_BACK;
    } else if (screen == SCR_SETTING) {
        if (inRect(x, y, ssidBoxR)) return BTN_SET_SSID;
        if (inRect(x, y, passBoxR)) return BTN_SET_PASS;
        // 所有键盘格子 1 格宽，逐个检测
        for (uint8_t i = 0; i < KB_COUNT; i++) {
            Rect r = kbCellRect(i);
            if (inRect(x, y, r)) return BTN_KB0 + i;
        }
    }
    return BTN_NONE;
}

static void pressVisual(uint8_t id, bool pressed) {
    if (id <= BTN_NUM9) { drawNumKey(id, pressed); return; }
    if (id == BTN_CONFIRM) { drawConfirmKey(pressed); return; }
    if (id >= BTN_ANS_A && id <= BTN_ANS_F) { drawAnsKey(id - BTN_ANS_A, pressed); return; }
    if (id == BTN_SEND) { drawSendKey(pressed); return; }
    if (id == BTN_BACK) { drawBackKey(pressed); return; }
    if (id >= BTN_KB0 && id <= BTN_KB_LAST) { drawKbCell(id - BTN_KB0, pressed); return; }
}

static void fireAction(uint8_t id) {
    if (screen == SCR_LOGIN) {
        if (id <= BTN_NUM9) { inputDigit(id); return; }
        if (id == BTN_CONFIRM) { onConfirm(); return; }
        if (id >= BTN_FIELD0 && id <= BTN_FIELD2) { tapField(id - BTN_FIELD0); return; }
        if (id == BTN_GEAR) { enterSetting(); return; }
    } else if (screen == SCR_ANSWER) {
        if (id >= BTN_CELL0 && id <= BTN_CELL9) { tapCell(id - BTN_CELL0); return; }
        if (id == BTN_ANS_A) { inputAnswer('A'); return; }
        if (id == BTN_ANS_B) { inputAnswer('B'); return; }
        if (id == BTN_ANS_C) { inputAnswer('C'); return; }
        if (id == BTN_ANS_D) { inputAnswer('D'); return; }
        if (id == BTN_ANS_T) { inputAnswer('T'); return; }
        if (id == BTN_ANS_F) { inputAnswer('F'); return; }
        if (id == BTN_SEND) { onSend(); return; }
        if (id == BTN_BACK) {
            screen = SCR_LOGIN;
            drawLoginScreen();
            return;
        }
    } else if (screen == SCR_SETTING) {
        if (id == BTN_SET_SSID) { setTarget = 0; drawSetBox(0); drawSetBox(1); return; }
        if (id == BTN_SET_PASS) { setTarget = 1; drawSetBox(0); drawSetBox(1); return; }
        if (id >= BTN_KB0 && id <= BTN_KB_LAST) {
            uint8_t idx = id - BTN_KB0;
            if (idx == KB_IDX_SHIFT) { toggleShift(); return; }
            if (idx == KB_IDX_SPACE) { inputChar(' '); return; }
            if (idx == KB_IDX_BACK) { inputBackspace(); return; }
            if (idx == KB_IDX_SAVE) { saveWifi(); return; }
            char c = kbCharAt(idx);
            if (c) inputChar(c);
            return;
        }
    }
}

static void pollTouch() {
    if (!touching && tLastRelease && millis() - tLastRelease < TOUCH_DEBOUNCE_MS) return;

    if (!touching) {
        uint16_t sx, sy;
        if (tpReadScreen(&sx, &sy)) {          // 无触摸时快速返回 false
            touching = true;
            activeBtn = hitTest(sx, sy);
            if (activeBtn != BTN_NONE) pressVisual(activeBtn, true);
        }
    } else if (!tpPressed()) {                  // 释放沿
        touching = false;
        tLastRelease = millis();
        if (activeBtn != BTN_NONE) {
            pressVisual(activeBtn, false);
            fireAction(activeBtn);
            activeBtn = BTN_NONE;
        }
    }
}

// ============================================================
//  持久化
// ============================================================
void loadPrefs() {
    prefs.begin("responder", true);
    ipField[0] = prefs.getString("ip1", "");
    ipField[1] = prefs.getString("ip2", "");
    ipField[2] = prefs.getString("ip3", "");
    submitCount = prefs.getULong("cnt", 0);
    String s = prefs.getString("wsid", "");
    String p = prefs.getString("wpas", "");
    prefs.end();
    if (s.length()) { strncpy(wifiSsid, s.c_str(), SET_TEXT_MAX); wifiSsid[SET_TEXT_MAX] = 0; }
    if (p.length()) { strncpy(wifiPass, p.c_str(), SET_TEXT_MAX); wifiPass[SET_TEXT_MAX] = 0; }
    if (ipField[0].length() && ipField[1].length() && ipField[2].length()) {
        snprintf(serverIP, sizeof(serverIP), "192.168.%s.%s:%s",
                 ipField[0].c_str(), ipField[1].c_str(), ipField[2].c_str());
    }
}

void savePrefs() {
    prefs.begin("responder", false);
    prefs.putString("ip1", ipField[0]);
    prefs.putString("ip2", ipField[1]);
    prefs.putString("ip3", ipField[2]);
    prefs.putULong("cnt", submitCount);
    prefs.end();
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n[APP] responder v6 boot");

    tpInit();                    // XPT2046 bit-bang 引脚（先于屏幕初始化，互不依赖）
    delay(50);
    tft.init();
    tft.setRotation(1);          // 横屏 320x240
    tft.fillScreen(C_BG);

    loadPrefs();
    Serial.printf("[APP] wifi ssid='%s' (空=未配置,需进设置界面输入)\n", wifiSsid);
    Serial.printf("[APP] server=%s count=%lu\n", serverIP, (unsigned long)submitCount);

    // 答题格子布局
    for (uint8_t i = 0; i < ANS_COUNT; i++) {
        cellR[i] = {int16_t(6 + i * 31), 10, 29, 44};
    }
    // 设置界面键盘：5x10 网格，布局由 kbCellRect() 计算，无需预存

    // ---- 开机触摸诊断模式：开机瞬间若按住屏幕，持续输出原始值 ----
    // 用于诊断触控是否工作：打开串口监视器(115200)，按住屏幕某处，
    // 按下复位键，松开前观察串口输出。Z 值随按下变化说明驱动正常。
    if (tpPressed()) {
        Serial.println(F("[TP] === 诊断模式：持续输出原始值，10 秒后退出 ==="));
        Serial.println(F("[TP] 格式: Z1 Z2 Z  X  Y   (Z>80 视为按下)"));
        uint32_t t0 = millis();
        while (millis() - t0 < 10000) {
            tpDebugDump();
            delay(200);
        }
        Serial.println(F("[TP] === 诊断模式结束 ==="));
    }

    // 清除 v2 旧校准数据（校准点位置已变，旧数据无效）
    // 通过版本标记识别：v3 用 "v3" 标记
    prefs.begin("touch", false);
    bool isV3 = prefs.getString("ver", "") == "v3";
    prefs.end();
    if (!isV3) {
        prefs.begin("touch", false);
        prefs.clear();
        prefs.putString("ver", "v3");
        prefs.end();
        Serial.println(F("[TP] 清除旧校准数据，需重新校准"));
    }

    // 首次开机或旧数据已清：无校准数据则进入触摸校准
    if (!tpLoadCal(nullptr)) {
        Serial.println(F("[TP] 无校准数据，进入校准流程"));
        runCalibration();
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("[WIFI] connecting to '%s'\n", wifiSsid);
    // 等待连接结果（最多 15 秒），让开机时就能知道是否连上
    uint32_t tWifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - tWifiStart < WIFI_TIMEOUT_MS) {
        delay(200);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] connected! ip=%s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("\n[WIFI] connect failed status=%d (1=未连 4=已连 6=密码错)\n", WiFi.status());
    }

    screen = SCR_LOGIN;
    drawLoginScreen();
}

void loop() {
    uint32_t now = millis();

    // 触摸轮询：50Hz 节流
    if (now - tTouchPoll >= TOUCH_POLL_MS) {
        tTouchPoll = now;
        pollTouch();
    }

    // WiFi 断线重连：5s 节流
    if (now - tWifiTick >= 1000) {
        tWifiTick = now;
        bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wifiLogged) {
            wifiLogged = true;
            Serial.printf("[WIFI] connected ip=%s\n", WiFi.localIP().toString().c_str());
        } else if (!conn && now - tReconnect > 5000) {
            tReconnect = now;
            WiFi.reconnect();
        }
    }

    // 临时消息到期：答题界面恢复状态栏 / 登录界面全屏重绘
    if (statusUntil && now >= statusUntil) {
        statusUntil = 0;
        if (screen == SCR_ANSWER) refreshStatus();
        else if (screen == SCR_LOGIN) drawLoginScreen();
    }

    delay(2);   // 让出 CPU
}
