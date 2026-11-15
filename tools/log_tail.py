#!/usr/bin/env python3
"""持续增量抓取板子 /api/log 到本地文件，用于复现 BLE 断连时留证据。

用法:
    python tools/log_tail.py [输出文件] [持续秒数]

板子的日志是 16KB 环形缓冲，但 /api/log?since=N 带序号，可增量读取。
本脚本每秒拉一次，把新增行带时间戳落盘；若序号回退说明板子重启过，
会明确记一行「板子重启」——这正是判断「掉压/崩溃重启 vs 纯链路超时」的关键。

浏览器/串口都不占，纯 HTTP，可与手机 App 同时运行。
"""
import datetime
import sys
import time
import urllib.request

URL = "http://192.168.0.102/api/log"
OUT = sys.argv[1] if len(sys.argv) > 1 else "ble_live_log.txt"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 900.0


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S")


def main():
    since = 0
    end = time.time() + SECS
    with open(OUT, "a", encoding="utf-8") as f:

        def w(line):
            f.write(line.rstrip("\n") + "\n")
            f.flush()

        w(f"=== 开始采集 {stamp()}，持续 {SECS:.0f}s，目标 {URL} ===")
        misses = 0
        while time.time() < end:
            try:
                req = urllib.request.Request(
                    URL + f"?since={since}", headers={"Connection": "close"}
                )
                with urllib.request.urlopen(req, timeout=3) as r:
                    body = r.read().decode("utf-8", "replace")
                    now = int(r.headers.get("X-Log-Now", "0") or 0)
                misses = 0
                if now < since:
                    w(f"[{stamp()}] ===== 板子重启：日志序号 {since} -> {now} =====")
                    since = 0
                elif now > since:
                    for line in body.splitlines():
                        w(f"[{stamp()}] {line}")
                    since = now
            except Exception as e:
                misses += 1
                if misses in (1, 5, 20, 60):
                    w(f"[{stamp()}] (HTTP 暂时不可达 #{misses}: {type(e).__name__})")
            time.sleep(1.0)
        w(f"=== 采集结束 {stamp()} ===")


if __name__ == "__main__":
    main()
