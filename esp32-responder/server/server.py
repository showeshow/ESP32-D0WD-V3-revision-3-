#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32 答题器最小服务器 — 实现 /submit 接口
用法：
  1. 在电脑上运行：python server.py [端口]
     默认端口 8080
  2. 答题器登录界面输入：电脑内网IP:端口（如 192.168.1.100:8080）
  3. 学生提交答案后，本脚本会打印收到的答案

确认电脑内网IP：
  Windows:  ipconfig   看 "IPv4 地址"
  答题器和电脑必须在同一WiFi/局域网。
"""
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import parse_qs

class Handler(BaseHTTPRequestHandler):
    def _send(self, code, text):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_POST(self):
        if self.path != "/submit":
            self._send(404, "Not Found: " + self.path)
            return
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        params = parse_qs(raw)
        answers = params.get("answers", [""])[0]
        seq = params.get("seq", ["?"])[0]
        print(f"[提交 #{seq}] {answers}")
        # 答案格式：A,B,C,D,T,F,...（10项，-表示未答）
        items = answers.split(",")
        print("  分解：", end="")
        for i, a in enumerate(items):
            if a == "-":
                label = "未答"
            elif a == "T":
                label = "对"
            elif a == "F":
                label = "错"
            else:
                label = a
            print(f"Q{i+1}={label}  ", end="")
        print()
        print("-" * 50)
        self._send(200, "OK")

    def do_GET(self):
        # 简单的连通性测试页
        if self.path == "/" or self.path == "/index.html":
            html = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>ESP32 答题服务器</title></head>
<body style="font-family:sans-serif;padding:20px">
<h1>答题服务器就绪</h1>
<p>答题器配置的地址正确。提交答案会在此控制台打印。</p>
</body></html>"""
            self._send(200, html)
        else:
            self._send(404, "Not Found")

    def log_message(self, fmt, *args):
        # 简化日志，只显示方法和路径
        print(f"[{self.client_address[0]}] {self.command} {self.path}")

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"=" * 50)
    print(f"ESP32 答题服务器已启动")
    print(f"监听端口: {port}")
    print(f"答题器登录界面输入: <本机IP>:{port}")
    print(f"等待答题器提交答案...")
    print(f"=" * 50)
    print(f"(Ctrl+C 退出)\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        server.server_close()

if __name__ == "__main__":
    main()
