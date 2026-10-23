#!/usr/bin/env python3
"""
ble_client.py — GATT client (Central) on Raspberry Pi 4 (Pi OS 64-bit, BlueZ) using bleak.

  - 掃描並連線 STM32 (GATT server)
  - 訂閱 ACC notify：6 bytes = int16 x, y, z (mg, little-endian)
  - 寫入 FREQ：2 bytes = uint16 Hz
  - 訂閱 MOTION notify（option b）：2 bytes = uint16 事件計數
  - 執行中在終端機輸入數字即可即時改頻率；斷線自動重連

Usage:
  python ble_client.py                    # 掃描名稱含 STM32 的裝置
  python ble_client.py --freq 20
  python ble_client.py --addr XX:XX:XX:XX:XX:XX --csv acc.csv
"""
import argparse
import asyncio
import csv
import struct
import sys
import threading
import time

from bleak import BleakClient, BleakScanner

ACC_UUID    = "7a1e0001-3c2b-4d5e-9f10-a1b2c3d4e5f6"
FREQ_UUID   = "7a1e0002-3c2b-4d5e-9f10-a1b2c3d4e5f6"
MOTION_UUID = "7a1e0003-3c2b-4d5e-9f10-a1b2c3d4e5f6"
SERVICE_UUID = "7a1e0000-3c2b-4d5e-9f10-a1b2c3d4e5f6"


class AccClient:
    def __init__(self, args):
        self.args = args
        self.client = None
        self.disconnected = asyncio.Event()
        self.rx_count = 0
        self.rx_rate_t0 = time.monotonic()
        self.csv_writer = None
        self.csv_file = None
        if args.csv:
            self.csv_file = open(args.csv, "w", newline="")
            self.csv_writer = csv.writer(self.csv_file)
            self.csv_writer.writerow(["time", "x_mg", "y_mg", "z_mg"])

    # ---------------- 掃描 ----------------
    async def find_device(self):
        while True:
            if self.args.addr:
                print(f"[scan] looking for {self.args.addr} ...")
                dev = await BleakScanner.find_device_by_address(self.args.addr, timeout=10.0)
            else:
                print(f"[scan] looking for name containing '{self.args.name}' ...")
                dev = await BleakScanner.find_device_by_filter(
                    lambda d, ad: (
                        (d.name and self.args.name in d.name)
                        or (ad.local_name and self.args.name in ad.local_name)
                        or SERVICE_UUID in [u.lower() for u in ad.service_uuids]
                    ),
                    timeout=10.0,
                )
            if dev:
                print(f"[scan] found {dev.name} ({dev.address})")
                return dev
            print("[scan] not found, retrying ...")

    # ---------------- notify handlers ----------------
    def on_acc(self, _sender, data: bytearray):
        if len(data) < 6:
            return
        x, y, z = struct.unpack("<hhh", bytes(data[:6]))
        now = time.time()
        self.rx_count += 1
        if not self.args.quiet:
            print(f"{now:.3f}  x={x:6d}  y={y:6d}  z={z:6d}  mg")
        if self.csv_writer:
            self.csv_writer.writerow([f"{now:.3f}", x, y, z])

        elapsed = time.monotonic() - self.rx_rate_t0
        if elapsed >= 1.0:
            print(f"[rate] {self.rx_count / elapsed:.1f} samples/s")
            self.rx_count = 0
            self.rx_rate_t0 = time.monotonic()

    def on_motion(self, _sender, data: bytearray):
        count = struct.unpack("<H", bytes(data[:2]))[0] if len(data) >= 2 else -1
        print(f"\n*** Significant motion detected (#{count}) ***\n")

    def on_disconnect(self, _client):
        print("[ble] disconnected")
        self.disconnected.set()

    # ---------------- FREQ ----------------
    async def set_freq(self, hz: int):
        if not self.client or not self.client.is_connected:
            print("[freq] not connected")
            return
        await self.client.write_gatt_char(FREQ_UUID, struct.pack("<H", hz), response=True)
        val = await self.client.read_gatt_char(FREQ_UUID)
        print(f"[freq] requested {hz} Hz, STM32 now = {struct.unpack('<H', bytes(val[:2]))[0]} Hz")

    async def stdin_loop(self):
        # 用 daemon thread 讀 stdin，Ctrl+C 時不會卡住等待 Enter
        loop = asyncio.get_running_loop()
        queue: asyncio.Queue = asyncio.Queue()

        def reader():
            for raw in sys.stdin:
                loop.call_soon_threadsafe(queue.put_nowait, raw)

        threading.Thread(target=reader, daemon=True).start()
        while True:
            line = (await queue.get()).strip()
            if line.isdigit():
                try:
                    await self.set_freq(int(line))
                except Exception as e:  # noqa: BLE001
                    print(f"[freq] write failed: {e}")
            elif line:
                print("輸入整數 (Hz) 以修改取樣頻率，Ctrl+C 結束")

    # ---------------- 主流程 ----------------
    async def run(self):
        stdin_task = asyncio.create_task(self.stdin_loop())
        try:
            while True:
                dev = await self.find_device()
                self.disconnected.clear()
                try:
                    async with BleakClient(dev, disconnected_callback=self.on_disconnect) as client:
                        self.client = client
                        print("[ble] connected")

                        if self.args.freq:
                            await self.set_freq(self.args.freq)

                        await client.start_notify(ACC_UUID, self.on_acc)
                        try:
                            await client.start_notify(MOTION_UUID, self.on_motion)
                        except Exception:  # noqa: BLE001
                            print("[ble] MOTION characteristic not available (option b disabled?)")

                        print("[ble] receiving ... 輸入數字 + Enter 可改頻率")
                        await self.disconnected.wait()
                except Exception as e:  # noqa: BLE001
                    print(f"[ble] error: {e}")
                finally:
                    self.client = None
                print("[ble] reconnecting in 2 s ...")
                await asyncio.sleep(2)
        finally:
            stdin_task.cancel()

    async def shutdown(self):
        if self.client and self.client.is_connected:
            for uuid in (ACC_UUID, MOTION_UUID):
                try:
                    await self.client.stop_notify(uuid)
                except Exception:  # noqa: BLE001
                    pass
            await self.client.disconnect()
        if self.csv_file:
            self.csv_file.close()


def main():
    p = argparse.ArgumentParser(description="STM32 BLE accelerometer GATT client")
    p.add_argument("--name", default="STM32", help="裝置名稱包含的字串 (default: STM32)")
    p.add_argument("--addr", help="直接指定 MAC address")
    p.add_argument("--freq", type=int, help="連線後設定的取樣頻率 (Hz, 1~100)")
    p.add_argument("--csv", help="把資料存成 CSV")
    p.add_argument("--quiet", action="store_true", help="不逐筆印出，只印速率")
    args = p.parse_args()

    app = AccClient(args)

    async def runner():
        try:
            await app.run()
        finally:
            await app.shutdown()

    try:
        asyncio.run(runner())
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
