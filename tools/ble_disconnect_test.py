#!/usr/bin/env python3
# 复现 BLE 连接“几秒后断开”问题：连上 -> 发一条命令 -> 空闲等待，报告断开原因/耗时。
import asyncio, sys, time
from bleak import BleakScanner, BleakClient

SVC  = "4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f"
CMD  = "4b9131c3-c9c5-cc8f-9e45-b51f02c2af4f"
STS  = "4b9131c3-c9c5-cc8f-9e45-b51f03c2af4f"

disconnected = asyncio.Event()
disc_reason = None
disc_time = None

def on_disconnect(client, reason=None):
    global disc_reason, disc_time
    disc_reason = reason
    disc_time = time.time()
    disconnected.set()
    print(f"[DISCONNECT] reason={reason} t={disc_time:.2f}", flush=True)

async def main():
    devs = await BleakScanner.discover(timeout=5)
    target = next((d for d in devs if (d.name or "").upper() == "SPIDER8"), None)
    if not target:
        print("未找到 Spider8，退出"); return 1
    print(f"找到 {target.name} {target.address}")
    t0 = time.time()
    async with BleakClient(target.address, timeout=20, disconnected_callback=on_disconnect) as cli:
        print(f"[CONNECT] {time.time()-t0:.2f}s")
        svc = cli.services.get_service(SVC)
        if not svc:
            print("未发现自定义服务"); return 2
        cmd = svc.get_characteristic(CMD)
        sts = svc.get_characteristic(STS)
        print(f"[SVC] cmd={cmd.uuid if cmd else None} sts={sts.uuid if sts else None}")
        # 开 notify
        if sts:
            try:
                await cli.start_notify(sts.uuid, lambda h,d: print(f"[NOTIFY] {d.decode(errors='replace')[:80]}"))
            except Exception as e:
                print(f"start_notify err {e}")
        # 发命令
        await cli.write_gatt_char(cmd.uuid, b"status\r\n", response=False)
        print(f"[SENT] status @ {time.time()-t0:.2f}s")
        # 空闲等待 25s，看是否断开
        wait = 25
        print(f"[IDLE] 等待 {wait}s ...")
        try:
            await asyncio.wait_for(disconnected.wait(), timeout=wait)
        except asyncio.TimeoutError:
            print(f"[OK] {wait}s 内未断开，连接稳定")
        dt = (disc_time - t0) if disc_time else None
        print(f"[SUMMARY] reason={disc_reason} elapsed={dt:.2f}s" if dt else f"[SUMMARY] 稳定")
    return 0

if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
