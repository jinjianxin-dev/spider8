#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""voice_oral_test.py — Spider8 语音口测记录器（I1/I2 通用）

持续监听 USB Serial/JTAG 串口，把语音模块的事件逐条记下来并统计：
  - 唤醒词命中次数
  - 每条命令词（1~25）命中次数 / 中文说法
  - 超时、表外命令、麦克风峰值分布

用法：
  python tools/voice_oral_test.py COM7 --seconds 25              # 安静环境自检
  python tools/voice_oral_test.py COM7 --seconds 600 --out oral.log  # 口测

注意（本项目踩过的坑）：
  - 必须插标 USB 的口（VID_303A），COM 号不固定，先探测。
  - pyserial 的 DTR/RTS 会误复位 ESP32-S3，open 前后都要置 False。
  - 本脚本会独占串口，运行期间不要再跑 serial_console.py / 烧录。
  - 所有文本显式 UTF-8，避免中文日志在 GBK 环境下读坏。
"""

import argparse
import re
import sys
import time
from datetime import datetime

import serial

# 与 firmware/main/voice.c 的 k_cmds[] 严格一致（id -> 中文）
CMDS = {
    1: "前进", 2: "后退", 3: "慢走", 4: "快走", 5: "快跑",
    6: "左转", 7: "右转", 8: "转个圈", 9: "直走", 10: "停止",
    11: "停下", 12: "别动", 13: "停", 14: "站好", 15: "站起来",
    16: "蹲下", 17: "趴下", 18: "握手", 19: "挥手", 20: "演示",
    21: "快一点", 22: "慢一点", 23: "大步", 24: "小步", 25: "查内存",
}

RE_WAKE = re.compile(r">>\s*唤醒词命中")
RE_HEARD = re.compile(r">>\s*heard:\s*(.+?)\s*\(id=(\d+)")
RE_TIMEOUT = re.compile(r">>\s*没听到命令")
RE_UNKNOWN = re.compile(r">>\s*表外命令\s*id=(\d+)")
RE_PEAK = re.compile(r"\[mic\]\s*peak=(\d+)")
RE_CLIP = re.compile(r"clip=([\d.]+)%")
RE_VAD = re.compile(r"\[vad\]\s*(SPEECH|silence)\s+vol=([-\d.]+)")
RE_EAR = re.compile(r"\[ear\]\s*(闭麦|开麦)")
RE_WN = re.compile(r"\[wn\]\s*state=(\d+)")
RE_BAD = re.compile(r"Guru Meditation|abort\(\)|assert failed|Brownout|rst:0x", re.I)


def main():
    ap = argparse.ArgumentParser(description="Spider8 语音口测记录器")
    ap.add_argument("port", nargs="?", default="COM7", help="串口，如 COM7")
    ap.add_argument("--seconds", type=int, default=300, help="监听秒数")
    ap.add_argument("--out", default="voice_oral_test.log", help="原始日志输出文件")
    ap.add_argument("--quiet", action="store_true", help="不实时打印事件行")
    args = ap.parse_args()

    try:
        sp = serial.Serial()
        sp.port = args.port
        sp.baudrate = 115200
        sp.timeout = 0.25
        sp.dtr = False
        sp.rts = False
        sp.open()
        sp.dtr = False
        sp.rts = False
    except Exception as e:
        print("OPEN_FAIL %s %s: %s" % (args.port, type(e).__name__, e), flush=True)
        return 2

    wakes = 0
    hits = {}
    timeouts = 0
    unknown = []
    peaks = []
    clips = []
    vols = []
    vad_speech = 0
    vad_silence = 0
    ear_closed = 0
    ear_open = 0
    wn_states = []
    bad = []
    events = []
    pending = b""
    start = time.time()
    end = start + args.seconds

    with open(args.out, "w", encoding="utf-8", newline="") as f:
        f.write("# Spider8 voice oral test  start=%s  port=%s  seconds=%d\n"
                % (datetime.now().isoformat(timespec="seconds"), args.port, args.seconds))
        print("LISTENING %s for %ds -> %s" % (args.port, args.seconds, args.out), flush=True)
        try:
            while time.time() < end:
                chunk = sp.read(4096)
                if chunk:
                    f.write(chunk.decode("utf-8", "replace"))
                    f.flush()
                    pending += chunk
                    while b"\n" in pending:
                        raw, pending = pending.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").strip()
                        if not line:
                            continue
                        stamp = "+%6.1fs" % (time.time() - start)
                        tag = None
                        if RE_WAKE.search(line):
                            wakes += 1
                            tag = "WAKE"
                        elif RE_HEARD.search(line):
                            m = RE_HEARD.search(line)
                            cid = int(m.group(2))
                            hits[cid] = hits.get(cid, 0) + 1
                            tag = "HIT  id=%-2d %s" % (cid, CMDS.get(cid, "?"))
                        elif RE_TIMEOUT.search(line):
                            timeouts += 1
                            tag = "TIMEOUT"
                        elif RE_UNKNOWN.search(line):
                            unknown.append(int(RE_UNKNOWN.search(line).group(1)))
                            tag = "UNKNOWN"
                        elif RE_PEAK.search(line):
                            peaks.append(int(RE_PEAK.search(line).group(1)))
                            mc = RE_CLIP.search(line)
                            if mc:
                                clips.append(float(mc.group(1)))
                            tag = "MIC"
                        elif RE_VAD.search(line):
                            mv = RE_VAD.search(line)
                            vols.append(float(mv.group(2)))
                            if mv.group(1) == "SPEECH":
                                vad_speech += 1
                            else:
                                vad_silence += 1
                            tag = "VAD"
                        elif RE_EAR.search(line):
                            if RE_EAR.search(line).group(1) == "闭麦":
                                ear_closed += 1
                                tag = "EAR-MUTE"
                            else:
                                ear_open += 1
                                tag = "EAR-LIVE"
                        elif RE_WN.search(line):
                            wn_states.append(int(RE_WN.search(line).group(1)))
                            tag = "WN"
                        if RE_BAD.search(line):
                            bad.append(line)
                            tag = "!!CRASH!!"
                        if tag:
                            events.append((stamp, tag, line))
                            if not args.quiet:
                                print("%s [%s] %s" % (stamp, tag, line), flush=True)
                if not chunk:
                    continue
        except KeyboardInterrupt:
            print("INTERRUPTED", flush=True)
        finally:
            sp.close()

    print("\n===== SUMMARY (%.1fs) =====" % (time.time() - start), flush=True)
    print("唤醒词命中 : %d" % wakes, flush=True)
    print("命令词命中 : %d 次 / %d 条不同词" % (sum(hits.values()), len(hits)), flush=True)
    for cid in sorted(hits):
        print("   id=%-2d %-6s x%d" % (cid, CMDS.get(cid, "?"), hits[cid]), flush=True)
    missed = [i for i in CMDS if i not in hits]
    print("未命中词   : %d 条  %s" % (len(missed), " ".join(map(str, missed))), flush=True)
    print("命令超时   : %d" % timeouts, flush=True)
    print("表外命令   : %d  %s" % (len(unknown), unknown), flush=True)
    if peaks:
        print("麦克风peak : n=%d  min=%d  max=%d  avg=%.0f"
              % (len(peaks), min(peaks), max(peaks), sum(peaks) / len(peaks)), flush=True)
    else:
        print("麦克风peak : (无采样，检查监听时长 >=5s)", flush=True)
    if clips:
        print("削顶比例   : n=%d max=%.2f%% avg=%.2f%%  (>0 即输入过载，用 voice gain 调大 shift)"
              % (len(clips), max(clips), sum(clips) / len(clips)), flush=True)
    if vols:
        print("AFE音量    : n=%d min=%.1f max=%.1f dBFS  | VAD: SPEECH=%d silence=%d"
              % (len(vols), min(vols), max(vols), vad_speech, vad_silence), flush=True)
    print("闭麦窗口   : 开=%d 关=%d  | 唤醒通道状态变化=%d %s"
          % (ear_open, ear_closed, len(wn_states), sorted(set(wn_states))), flush=True)
    print("崩溃/重启  : %d %s" % (len(bad), bad[:3]), flush=True)
    print("原始日志   : %s" % args.out, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
