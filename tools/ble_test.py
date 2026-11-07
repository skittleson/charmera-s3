#!/usr/bin/env python3
"""End-to-end check of the Bluetooth remote from a PC (needs `bleak`).

    /usr/bin/python3 tools/ble_test.py            # full run: looks, errors, a photo, a short time-lapse
    /usr/bin/python3 tools/ble_test.py --status   # just print Info and Status

Leaves the camera on the Normal look, single shot, 10 s interval. Takes 1 photo and
a ~15 s time-lapse at 5 s (a new DCIM/NNNLAPSE folder).
"""
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = "-7b6e-4f0a-9d2e-5a1c8e3f6b20"
SERVICE, STATUS, COMMAND, INFO = (f"c4a3000{n}{BASE}" for n in range(4))

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail and not ok else ""))


class Camera:
    def __init__(self, client):
        self.c = client
        self.status = {}
        self.seen = []  # every status received, in order

    def on_status(self, _char, data):
        try:
            self.status = json.loads(data.decode())
            self.seen.append(self.status)
        except ValueError:
            pass  # truncated notification; read() below gets the full value

    async def read(self):
        self.status = json.loads((await self.c.read_gatt_char(STATUS)).decode())
        return self.status

    async def send(self, text):
        await self.c.write_gatt_char(COMMAND, text.encode(), response=True)

    async def wait(self, pred, timeout=10.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if pred(self.status):
                return True
            await asyncio.sleep(0.1)
        await self.read()
        return pred(self.status)


async def main():
    print("scanning for Charmera-S3...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE in [u.lower() for u in ad.service_uuids], timeout=15)
    if not dev:
        sys.exit("camera not found (is Bluetooth on in Settings, and nothing else connected?)")
    async with BleakClient(dev) as client:
        cam = Camera(client)
        print(f"connected to {dev.name} {dev.address}, mtu {client.mtu_size}")
        info = json.loads((await client.read_gatt_char(INFO)).decode())
        await client.start_notify(STATUS, cam.on_status)
        st = await cam.read()
        if "--status" in sys.argv:
            print(json.dumps(info, indent=1))
            print(json.dumps(st, indent=1))
            return

        check("info lists 12 looks and 5 intervals", len(info["looks"]) == 12 and len(info["intervals"]) == 5,
              str(info))
        check("status has every field", {"photos", "left", "battery", "usb", "camera", "look", "lookName", "mode",
                                         "interval", "intervalName", "running", "frames", "elapsed", "folder",
                                         "card", "busy", "last", "error", "clock", "orientation", "orientationSetting"} <= set(st), str(st))
        print(f"      camera {st['camera']}, {st['photos']} photos, {st['left']} left, battery {st['battery']}%"
              f"{' (USB)' if st['usb'] else ''}, card {st['card']}")

        await cam.send(f"time {int(time.time())} {-time.timezone // 60 if not time.localtime().tm_isdst else -time.altzone // 60}")
        check("time -> clock set", await cam.wait(lambda s: s.get("clock") is True), str(cam.status))
        await cam.send("time 12")
        check("time 12 -> bad time", await cam.wait(lambda s: s["error"] == "bad time"), str(cam.status))

        await cam.send("look 5")
        check("look 5 -> notified", await cam.wait(lambda s: s["look"] == 5 and s["lookName"] == info["looks"][5]),
              str(cam.status))
        await cam.send("  LOOK 3\r\n")
        check("'  LOOK 3\\r\\n' tolerated", await cam.wait(lambda s: s["look"] == 3), str(cam.status))
        await cam.send("orientation landscape-right")
        check("orientation landscape-right", await cam.wait(
            lambda s: s["orientation"] == "landscape-right" and s["orientationSetting"] == "landscape-right"),
              str(cam.status))
        await cam.send("orientation sideways")
        check("orientation sideways -> bad orientation", await cam.wait(lambda s: s["error"] == "bad orientation"),
              str(cam.status))
        await cam.send("orientation portrait")
        check("orientation portrait", await cam.wait(lambda s: s["orientation"] == "portrait"), str(cam.status))

        await cam.send("look 99")
        check("look 99 -> bad look, look unchanged",
              await cam.wait(lambda s: s["error"] == "bad look" and s["look"] == 3), str(cam.status))
        await cam.send("dance")
        check("dance -> unknown command", await cam.wait(lambda s: s["error"] == "unknown command"), str(cam.status))
        await cam.send("mode lapse 0")
        check("mode lapse 0 -> lapse, 5 s, error cleared",
              await cam.wait(lambda s: s["mode"] == "lapse" and s["interval"] == 0 and s["error"] == ""),
              str(cam.status))
        await cam.send("mode single")
        check("mode single", await cam.wait(lambda s: s["mode"] == "single"), str(cam.status))

        before = cam.status["photos"]
        cam.seen.clear()
        await cam.send("shoot")
        ok = await cam.wait(lambda s: s["photos"] == before + 1 and not s["busy"] and s["last"].startswith("IMG_"),
                            timeout=25)
        check("shoot -> photo saved, count +1", ok, str(cam.status))
        check("shoot -> busy was reported while saving", any(s["busy"] for s in cam.seen))
        print(f"      saved {cam.status['last']}")

        await cam.send("lapse start")
        check("lapse start -> running", await cam.wait(lambda s: s["running"]), str(cam.status))
        check("lapse reached 2 frames", await cam.wait(lambda s: s["frames"] >= 2, timeout=30), str(cam.status))
        await cam.send("look 1")
        check("look during run -> refused, run continues",
              await cam.wait(lambda s: s["error"] == "time-lapse running" and s["running"] and s["look"] == 3),
              str(cam.status))
        await cam.send("orientation auto")
        check("orientation during run -> refused",
              await cam.wait(lambda s: s["error"] == "time-lapse running" and s["orientationSetting"] == "portrait"),
              str(cam.status))
        await cam.send("shoot")
        check("shoot during run -> refused", await cam.wait(lambda s: s["error"] == "time-lapse running"),
              str(cam.status))
        folder = cam.status["folder"]
        await cam.send("lapse stop")
        check("lapse stop -> stopped", await cam.wait(lambda s: not s["running"], timeout=20), str(cam.status))
        print(f"      time-lapse frames went to {folder}")
        await cam.send("lapse stop")
        check("lapse stop again -> not running", await cam.wait(lambda s: s["error"] == "not running"),
              str(cam.status))

        for cmd in ("look 0", "interval 1", "mode single"):
            await cam.send(cmd)
        check("restored Normal / 10 s / single",
              await cam.wait(lambda s: s["look"] == 0 and s["interval"] == 1 and s["mode"] == "single"),
              str(cam.status))
    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


asyncio.run(main())
