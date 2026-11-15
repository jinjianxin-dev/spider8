#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""直接扫描 Spider8 并打印它暴露的所有 GATT 服务/特征值 UUID。
用于确认固件侧自定义服务是否真的注册进了 GATT 数据库。
"""
import asyncio
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "Spider8"
SVC_TARGET = "4b9131c3-c9c5-cc8f-9e45-b51f01c2af4f"


async def main():
    print(f"[1/3] 扫描 {DEVICE_NAME} ...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15)
    if dev is None:
        print("✗ 未发现 Spider8：确认板子已上电、蓝牙已开、VPN 已退")
        return
    print(f"  ✔ 发现 {dev.address}  ({dev.name})")

    print("[2/3] 连接并发现服务 ...")
    async with BleakClient(dev, timeout=20) as client:
        services = list(client.services)
        print(f"  共 {len(services)} 个服务：\n")
        found = False
        for s in services:
            mark = "  <== 目标自定义服务" if s.uuid.lower() == SVC_TARGET else ""
            if mark:
                found = True
            print(f"  SVC  {s.uuid}{mark}")
            for c in s.characteristics:
                props = ",".join(c.properties)
                print(f"        CHR {c.uuid}  [{props}]")
        print()
        if found:
            print("✔ 目标服务 4b9131c3-...-01c2af4f 已存在于 GATT 数据库")
        else:
            print("✗ 目标服务不在 GATT 数据库 —— 固件注册有问题（或板子跑的不是最新固件）")


if __name__ == "__main__":
    asyncio.run(main())
