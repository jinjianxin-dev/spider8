"""打开串口读几秒日志，结果写文件（避免 PowerShell 管道捕获问题）。

用法: python probe_serial.py COM8 5
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
out = sys.argv[3] if len(sys.argv) > 3 else "_boot.log"

try:
    s = serial.Serial(port, 115200, timeout=0.3)
except Exception as exc:                      # noqa: BLE001
    print("OPEN_FAIL %s: %s" % (type(exc).__name__, exc))
    sys.exit(1)

buf = bytearray()
end = time.time() + secs
try:
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf.extend(chunk)
finally:
    s.close()

with open(out, "wb") as fh:
    fh.write(buf)

print("OPEN_OK bytes=%d" % len(buf))
