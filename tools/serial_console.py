"""Spider8 串口控制台工具（不依赖 idf.py monitor，便于自动化抓取）。

关键点：pyserial 打开端口时默认会拉低 DTR/RTS，而 ESP32-S3 的 USB Serial/JTAG
把 DTR->EN(复位)、RTS->GPIO0(下载模式) 映射上去。若不立刻释放，芯片会被一直按在
复位里，表现为"端口能打开但一个字都收不到"。所以打开后马上把两条线置为非激活。

用法:
    python serial_console.py COM7 --boot            复位后抓 6 秒启动日志
    python serial_console.py COM7 help status       依次发命令并打印回复
    python serial_console.py COM7 --wait 8 pwmscan all   每条命令后多等 8 秒（给长命令留时间）
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
ARGS = list(sys.argv[2:])

# --wait <秒>      ：每条命令后等串口安静的最长时间（默认 2.5s）
# --hold <秒>      ：发命令后先干等一会儿再读，给 pwmscan 这类长命令留时间
# --pause <秒>     ：两条命令之间额外停一会儿，方便人眼看 LED 亮度变化
# --repeat <次数>  ：整串命令重复多少轮（默认 1）
WAIT = 2.5
HOLD = 0.0
PAUSE = 0.0
REPEAT = 1
while len(ARGS) >= 2 and ARGS[0].startswith("--"):
    if ARGS[0] == "--wait":
        WAIT = float(ARGS[1])
    elif ARGS[0] == "--hold":
        HOLD = float(ARGS[1])
    elif ARGS[0] == "--pause":
        PAUSE = float(ARGS[1])
    elif ARGS[0] == "--repeat":
        REPEAT = int(ARGS[1])
    else:
        break
    ARGS = ARGS[2:]


def open_port() -> serial.Serial:
    sp = serial.Serial()
    sp.port = PORT
    sp.baudrate = 115200
    sp.timeout = 0.2
    sp.write_timeout = 1.0
    sp.dtr = False
    sp.rts = False
    sp.open()
    sp.dtr = False
    sp.rts = False
    return sp


def drain(sp: serial.Serial, secs: float) -> str:
    """读到串口安静为止，最多等 secs 秒。

    注意串口持续处于"安静"状态时不能一直等，所以用一个 idle 计时：
    每收到数据就把截止时间往回推，但不超过 secs 的上限。
    """
    buf = bytearray()
    deadline = time.time() + secs
    idle_end = time.time() + 0.6          # 连续 0.6 秒没数据就算说完了
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            buf.extend(chunk)
            idle_end = time.time() + 0.6
        elif time.time() > idle_end:
            break
    return buf.decode("utf-8", "replace")


def main() -> int:
    sp = open_port()
    try:
        if ARGS and ARGS[0] == "--boot":
            # 拉一下 EN 制造一次干净复位，然后抓启动日志
            sp.dtr = True
            time.sleep(0.15)
            sp.dtr = False
            time.sleep(0.05)
            print(drain(sp, 6.0))
            return 0

        time.sleep(0.3)
        drain(sp, 0.4)                      # 丢掉上电残留
        for r in range(REPEAT):
            if REPEAT > 1:
                print("===== 第 %d/%d 轮 =====" % (r + 1, REPEAT))
            for cmd in ARGS:
                sp.write((cmd + "\r\n").encode())
                sp.flush()
                print("$ " + cmd)
                if HOLD > 0:
                    time.sleep(HOLD)
                print(drain(sp, WAIT).rstrip())
                print()
                if PAUSE > 0:
                    time.sleep(PAUSE)
        return 0
    finally:
        sp.close()


if __name__ == "__main__":
    sys.exit(main())
