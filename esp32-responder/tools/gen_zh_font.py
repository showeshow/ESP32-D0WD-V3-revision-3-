# -*- coding: utf-8 -*-
"""生成答题器所需的 24x24 中文点阵字库 (zh_font.c)
仅包含界面用到的汉字，每个字 72 字节，总计约 3KB flash。
用法: python gen_zh_font.py
"""
from PIL import Image, ImageDraw, ImageFont

CHARS = "确认发送网络已连接断开提交次上成功失败正在测试中题答案内服务器端口地址请等待用户触摸屏校准新号数键对错重清空输入完整填不未响应超时无效就绪设备开始一页密码名点击左下角完保存格退大小写设返回先选择"

SIZE = 24
FONT = "C:/Windows/Fonts/simhei.ttf"

def render(ch, font):
    img = Image.new("L", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(img)
    bb = d.textbbox((0, 0), ch, font=font)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    if w <= 0 or h <= 0:
        return [0] * (SIZE * SIZE // 8)
    x = (SIZE - w) // 2 - bb[0]
    y = (SIZE - h) // 2 - bb[1]
    d.text((x, y), ch, font=font, fill=255)
    px = img.load()
    bits = []
    for row in range(SIZE):
        for byte_i in range(3):
            b = 0
            for bit in range(8):
                col = byte_i * 8 + bit
                b = (b << 1) | (1 if px[col, row] > 96 else 0)
            bits.append(b)
    return bits

def main():
    # 去重且保持顺序
    seen = set()
    uniq = []
    for c in CHARS:
        if c not in seen:
            seen.add(c)
            uniq.append(c)
    font = ImageFont.truetype(FONT, SIZE - 3)
    data = []
    for ch in uniq:
        data.extend(render(ch, font))
    lines = []
    lines.append('// 自动生成的 24x24 中文点阵字库 (simhei)，由 tools/gen_zh_font.py 生成')
    lines.append('// 每字 72 字节 (24行 x 3字节 MSB first)，仅供答题器界面文字使用')
    lines.append('#include <Arduino.h>')
    lines.append('')
    idx = "".join(uniq)
    lines.append('// C++ const 需显式外部链接：先 extern 声明再定义')
    lines.append('extern const char ZH_INDEX[];')
    lines.append(f'const char ZH_INDEX[] = "{idx}"; // {len(uniq)} 个汉字')
    lines.append('extern const uint8_t ZH_FONT[];')
    lines.append(f'const uint8_t ZH_FONT[{len(data)}] = {{')
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i+12])
        lines.append(f'    {chunk},')
    lines.append('};')
    with open("src/zh_font.c", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"OK: {len(uniq)} chars, {len(data)} bytes -> src/zh_font.c")

if __name__ == "__main__":
    main()
