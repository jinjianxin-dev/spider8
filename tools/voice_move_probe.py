#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""voice_move_probe.py — Spider8 I2 语音行走验收探针

一次串口连接内完成三个阶段，避免脚本抢占 COM 口：
  1) SIM   ：发 `voice sim <id>`，验证「识别结果 -> cli_exec -> 步态」链路，
             并观察 5s 行走超时是否自动站定。
  2) ORAL  ：留出真人说话窗口，记录唤醒/命中/执行/急停日志。
  3) STATUS：发 `voice` 读最终状态（exec / 已执行 / 急停 / 超时倒计时）。

用法：
  python tools/voice_move_probe.py COM7 --sim 1 --sim-secs 11 --oral-secs 40 \
      --out firmware/build/i2_walk_probe.log

注意（本项目踩过的坑）：
  - 只插标 USB 的口（VID_303A）；本脚本独占串口，运行期间不要烧录或另开控制台。
  - pyserial 的 DTR/RTS 会误复位 ESP32-S3，open 前后都要置 False。
  - 所有文本显式 UTF-8，避免中文日志在 GBK 环境下读坏。
"""

import argparse
import os
import re
import sys
import time
from datetime import datetime

import serial

# 摘要里值得单独拎出来的事件
RE_KEY = re.compile(
    r">> heard|\[voice->cli\]|\[voice->STOP\]|\[voice\]|唤醒词命中|没听到命令|"
    r"表外命令|动作放行|语音状态|Guru Meditation|abort\(\)|assert failed|"
    r"Brownout|rst:0x|5s 行走超时|已执行=",
    re.I,
)
RE_BAD = re.compile(r"Guru Meditation|abort\(\)|assert failed|Brownout|rst:0x", re.I)


def open_port(port: str) -> serial.Serial:
    sp = serial.Serial()
    sp.port = port
    sp.baudrate = 115200
    sp.timeout = 0.25
    sp.dtr = False
    sp.rts = False
    sp.open()
    sp.dtr = False
    sp.rts = False
    return sp


def collect(sp: serial.Serial, secs: float, lines: list, tag: str, pending: list) -> None:
    """按行收集串口输出，保留跨 read() 的半行。"""
    end = time.time() + secs
    while time.time() < end:
        chunk = sp.read(4096)
        if chunk:
            pending[0] += chunk.decode("utf-8", "replace")
            parts = pending[0].split("\n")
            pending[0] = parts.pop()
            for ln in parts:
                ln = ln.rstrip("\r")
                if ln.strip():
                    stamp = datetime.now().strftime("%H:%M:%S")
                    lines.append("[%s %s] %s" % (stamp, tag, ln))
        else:
            time.sleep(0.02)


def main() -> int:
    ap = argparse.ArgumentParser(description="Spider8 I2 语音行走验收探针")
    ap.add_argument("port", nargs="?", default="COM7")
    ap.add_argument("--sim", type=int, default=1, help="模拟命令词 id（1=前进）")
    ap.add_argument("--sim-secs", type=float, default=11.0, help="模拟后观察秒数（需 >5s 才看得到超时）")
    ap.add_argument("--oral-secs", type=float, default=40.0, help="真人说话窗口秒数")
    ap.add_argument("--out", default="firmware/build/i2_walk_probe.log")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    try:
        sp = open_port(args.port)
    except Exception as e:
        print("OPEN_FAIL %s %s: %s" % (args.port, type(e).__name__, e), flush=True)
        return 2

    lines: list = []
    pending = [""]
    try:
        time.sleep(0.3)
        collect(sp, 0.5, lines, "BOOT", pending)

        cmd = "voice sim %d" % args.sim
        sp.write((cmd + "\r\n").encode())
        sp.flush()
        lines.append("[%s SIM] $ %s" % (datetime.now().strftime("%H:%M:%S"), cmd))
        collect(sp, args.sim_secs, lines, "SIM", pending)

        lines.append("[%s ORAL] === 真语音窗口开始，请说「你好小智 -> 前进」===" %
                     datetime.now().strftime("%H:%M:%S"))
        collect(sp, args.oral_secs, lines, "ORAL", pending)

        sp.write(b"voice\r\n")
        sp.flush()
        lines.append("[%s STATUS] $ voice" % datetime.now().strftime("%H:%M:%S"))
        collect(sp, 2.0, lines, "STATUS", pending)
    finally:
        sp.close()
        if pending[0].strip():
            lines.append("[END] " + pending[0].rstrip())

    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    keys = [ln for ln in lines if RE_KEY.search(ln)]
    bads = [ln for ln in lines if RE_BAD.search(ln)]
    print("=== 日志已写入 %s（共 %d 行）===" % (out, len(lines)))
    print("=== 关键事件 ===")
    for ln in keys:
        print(ln)
    print("=== 崩溃/重启线索：%d 条 ===" % len(bads))
    for ln in bads:
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
