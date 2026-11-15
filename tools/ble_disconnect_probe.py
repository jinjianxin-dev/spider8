#!/usr/bin/env python3
"""复现 BLE「连上几秒后断开」，并同步抓板子串口日志（含断连 reason 码）。

用法:
    python tools/ble_disconnect_probe.py COM7 25

输出:
    - BLE 侧：连接/服务/命令/断开时间线
    - 串口侧：板子打印的 `BLE 断开 ... reason=xx`（HCI 错误码，诊断金标准）

注意：pyserial 打开 USB-Serial/JTAG 时默认会拉 DTR 触发 EN 复位，
      必须在 open 前后都把 dtr/rts 置 False，否则会把板子复位、测试失真。
"""
import asyncio
import sys
import threading
import time

from bleak import BleakClient, BleakScanner
import serial

SVC = "4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f"
CMD = "4b9131c3-c9c5-cc8f-9e45-b51f02c2af4f"
STS = "4b9131c3-c9c5-cc8f-9e45-b51f03c2af4f"

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
# 默认发不带换行的真实命令（固件 cli_exec 不吃尾部 \r\n，带了会回「未知命令」）
CMD_TEXT = sys.argv[3] if len(sys.argv) > 3 else "status"
# 是否模拟手机 App 的「带响应写」(WRITE_TYPE_DEFAULT)；0 = write without response
WRITE_WITH_RESPONSE = (sys.argv[4] != "0") if len(sys.argv) > 4 else True

stop_evt = threading.Event()
serial_buf = bytearray()
serial_err = []


def serial_reader():
    try:
        sp = serial.Serial()
        sp.port = PORT
        sp.baudrate = 115200
        sp.timeout = 0.2
        sp.dtr = False
        sp.rts = False
        sp.open()
        sp.dtr = False
        sp.rts = False
        while not stop_evt.is_set():
            chunk = sp.read(4096)
            if chunk:
                serial_buf.extend(chunk)
        sp.close()
    except Exception as e:  # noqa: BLE001
        serial_err.append(repr(e))


disconnected = asyncio.Event()
disc_t = None
t0 = None


def on_disconnect(client, *args):
    global disc_t
    disc_t = time.time()
    print(f"[DISCONNECT] t={disc_t - t0:.2f}s args={args}", flush=True)
    disconnected.set()


async def main():
    global t0
    th = threading.Thread(target=serial_reader, daemon=True)
    th.start()
    time.sleep(1.0)  # 让串口先稳定，避免 open 时的抖动影响 BLE

    devs = await BleakScanner.discover(timeout=5)
    target = next((d for d in devs if (d.name or "").upper() == "SPIDER8"), None)
    if not target:
        print("未找到 Spider8，退出")
        stop_evt.set()
        return 1
    print(f"找到 {target.name} {target.address}", flush=True)

    t0 = time.time()
    try:
        async with BleakClient(target.address, timeout=20,
                               disconnected_callback=on_disconnect) as cli:
            print(f"[CONNECT] {time.time() - t0:.2f}s", flush=True)
            svc = cli.services.get_service(SVC)
            if not svc:
                print("未发现自定义服务")
                stop_evt.set()
                return 2
            cmd = svc.get_characteristic(CMD)
            sts = svc.get_characteristic(STS)
            print(f"[SVC] cmd={cmd.uuid if cmd else None} sts={sts.uuid if sts else None}",
                  flush=True)

            if sts:
                try:
                    await cli.start_notify(
                        sts.uuid,
                        lambda h, d: print(f"[NOTIFY] {d.decode(errors='replace')[:100]}",
                                           flush=True))
                    print("[NOTIFY] 已订阅", flush=True)
                except Exception as e:
                    print(f"[NOTIFY] start_notify 失败: {e}", flush=True)

            try:
                print(f"[MTU] 本机协商后 mtu_size = {cli.mtu_size}", flush=True)
            except Exception:
                pass
            payload = CMD_TEXT.encode()
            await cli.write_gatt_char(cmd.uuid, payload, response=WRITE_WITH_RESPONSE)
            print(f"[SENT] {CMD_TEXT!r} (response={WRITE_WITH_RESPONSE}) @ "
                  f"{time.time() - t0:.2f}s", flush=True)

            print(f"[IDLE] 空闲观察 {WAIT}s ...", flush=True)
            try:
                await asyncio.wait_for(disconnected.wait(), timeout=WAIT)
            except asyncio.TimeoutError:
                print(f"[OK] {WAIT}s 内未断开，本机蓝牙连接稳定", flush=True)

            if disc_t:
                print(f"[SUMMARY] 断开耗时 {disc_t - t0:.2f}s", flush=True)
            else:
                print("[SUMMARY] 未断开（稳定）", flush=True)

            # 再发一条命令，确认断开前链路确实活着
            if not disconnected.is_set():
                try:
                    await cli.write_gatt_char(cmd.uuid, CMD_TEXT.encode(),
                                              response=WRITE_WITH_RESPONSE)
                    print("[ALIVE] 空闲后仍能写命令", flush=True)
                    await asyncio.sleep(1.5)
                except Exception as e:
                    print(f"[ALIVE] 空闲后写命令失败: {e}", flush=True)
    except Exception as e:
        print(f"[BLE-EXCEPTION] {type(e).__name__}: {e}", flush=True)

    stop_evt.set()
    th.join(timeout=2)
    return 0


if __name__ == "__main__":
    rc = asyncio.run(main())
    txt = bytes(serial_buf).decode("utf-8", "replace")
    print("\n===== 板子串口日志（BLE 相关） =====")
    keep = ("ble_ctl", "NimBLE", "BLE ", "GAP", "GATT", "advertise", "assert",
            "Rebooting", "Guru", "panic", "abort", "heap", "断")
    for line in txt.splitlines():
        if any(k in line for k in keep):
            print(line)
    if serial_err:
        print("串口监听异常:", serial_err)
    print(f"===== 串口共收到 {len(serial_buf)} 字节 =====")
    # 完整日志落盘，便于事后翻
    with open("ble_disconnect_serial.log", "wb") as f:
        f.write(bytes(serial_buf))
    sys.exit(rc or 0)
