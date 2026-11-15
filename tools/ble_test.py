#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Spider8 BLE 端到端验证脚本（模拟安卓客户端）。

前提：
  1. 板子已烧录「含 BLE 的新固件」并上电（蓝牙名 Spider8）。
  2. 运行本脚本的电脑有可用蓝牙（BLE），且已退出会劫持路由的 VPN。
  3. 安装依赖：  pip install bleak

用法：
  python ble_test.py                # 扫描并连 Spider8，依次发 walk 50 / status / stop
  python ble_test.py --cmd "left"   # 只发一条自定义命令

脚本会打印：发现设备 -> 连接 -> 发现服务 -> 开启状态通知 -> 写命令 -> 收到板子回话。
任一环节失败都会抛出明确错误，便于定位是固件、UUID 还是环境的问题。
"""

import argparse
import asyncio
import sys

from bleak import BleakScanner, BleakClient

# 必须与固件 main/ble_ctl.c 中的 SPIDER8_*_UUID 完全一致
DEVICE_NAME = "Spider8"
SVC_UUID = "4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f"
CMD_UUID = "4b9131c3-c9c5-cc8f-9e45-b51f02c2af4f"
STS_UUID = "4b9131c3-c9c5-cc8f-9e45-b51f03c2af4f"


def on_sts(_handle: int, data: bytearray):
    try:
        text = bytes(data).decode("utf-8", errors="replace")
    except Exception:
        text = repr(bytes(data))
    # 板子按 MTU 分包 notify，逐包打印即可看出完整性
    print(f"  [STS] {text!r}")


async def run(commands):
    print(f"[1/4] 扫描 {DEVICE_NAME} ...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=12)
    if device is None:
        print("✗ 未找到 Spider8：确认板子已刷 BLE 固件并上电、蓝牙已开、VPN 已退")
        return False
    print(f"  ✔ 发现：{device.address}  ({device.name})")

    print("[2/4] 连接并发现服务 ...")
    async with BleakClient(device, timeout=20) as client:
        svc = client.services.get_service(SVC_UUID)
        if svc is None:
            print("✗ 未找到 Spider8 自定义服务，UUID 不匹配？")
            return False
        if svc.get_characteristic(CMD_UUID) is None or svc.get_characteristic(STS_UUID) is None:
            print("✗ CMD / STS 特征值缺失")
            return False
        print("  ✔ 服务与特征值 OK")

        print("[3/4] 开启状态通知 (STS) ...")
        await client.start_notify(STS_UUID, on_sts)
        await asyncio.sleep(0.3)

        print("[4/4] 发送命令并等待回话（每条 1.2s）...")
        for cmd in commands:
            print(f"  → 发送：{cmd}")
            await client.write_gatt_char(
                CMD_UUID, cmd.encode("utf-8"), response=False
            )
            await asyncio.sleep(1.2)

        print("  断开 ...")
        await client.stop_notify(STS_UUID)

    print("✔ 验证完成：如果上面 [STS] 行显示了命令回显，说明 BLE 链路通畅。")
    return True


def main():
    ap = argparse.ArgumentParser(description="Spider8 BLE 验证")
    ap.add_argument(
        "--cmd", default=None,
        help="只发这一条命令（默认依次发 walk 50 / status / stop）",
    )
    args = ap.parse_args()

    if args.cmd:
        commands = [args.cmd]
    else:
        commands = ["walk 50", "status", "stop"]

    try:
        ok = asyncio.run(run(commands))
    except Exception as e:  # noqa: BLE001
        print(f"✗ 运行异常：{e}")
        sys.exit(1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
