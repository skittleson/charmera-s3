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
SERVICE, STATUS, COMMAND, INFO, STREAM = (f"c4a3000{n}{BASE}" for n in range(5))

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


async def fetch_file(client, command, timeout=60):
    """Run list/thumb/get over the STREAM characteristic. Bytes, or None if refused."""
    got = {"buf": bytearray(), "end": None, "err": None}
    done = asyncio.Event()

    def on_stream(_char, data):
        kind = data[0]
        if kind == 1:
            got["buf"] += data[1:]
        elif kind == 2:
            got["end"] = int.from_bytes(data[1:5], "big")
            done.set()
        elif kind == 3:
            got["err"] = bytes(data[1:]).decode()
            done.set()

    await client.start_notify(STREAM, on_stream)
    try:
        await client.write_gatt_char(COMMAND, command.encode(), response=True)
        await asyncio.wait_for(done.wait(), timeout)
    except asyncio.TimeoutError:
        return None
    finally:
        await client.stop_notify(STREAM)
    if got["err"] or got["end"] != len(got["buf"]):
        return None
    return bytes(got["buf"])


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

        # File service: the new photo is listed, its preview streams as a JPEG, and a
        # missing photo is refused with an error frame.
        shot = int(cam.status["last"][4:])
        blob = await fetch_file(client, "list")
        listed = json.loads(blob) if blob else []
        check("list -> includes the new photo", any(n == shot for n, _ in listed), str(listed[-3:]))
        thumb = await fetch_file(client, f"thumb {shot}")
        check("thumb -> small JPEG sidecar", bool(thumb) and thumb[:2] == b"\xff\xd8" and len(thumb) < 40000,
              f"{len(thumb or b'')} bytes")
        check("get 9999 -> refused", await fetch_file(client, "get 9999") is None)

        await cam.send("lapse start")
        check("lapse start -> running", await cam.wait(lambda s: s["running"]), str(cam.status))
        check("lapse reached 2 frames", await cam.wait(lambda s: s["frames"] >= 2, timeout=30), str(cam.status))
        await cam.send("look 1")
        # Like stop, a refusal is only reported once the in-flight (slow-look) frame is saved.
        check("look during run -> refused, run continues",
              await cam.wait(lambda s: s["error"] == "time-lapse running" and s["running"] and s["look"] == 3,
                             timeout=30),
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
        # Stop lands after the in-flight frame finishes (a save must complete
        # atomically). On a slow look a frame can run 15-20 s, more with
        # capture retries — so allow 45 s.
        check("lapse stop -> stopped", await cam.wait(lambda s: not s["running"], timeout=45), str(cam.status))
        print(f"      time-lapse frames went to {folder}")
        await cam.send("lapse stop")
        check("lapse stop again -> not running", await cam.wait(lambda s: s["error"] == "not running"),
              str(cam.status))

        # Video: mode video -> shoot starts a clip, refusals say "recording",
        # lapse stop commits it, and the clip appears in list.
        await cam.send("mode video")
        check("mode video", await cam.wait(lambda s: s["mode"] == "video"), str(cam.status))
        await cam.send("look 1")
        check("look during video mode (idle) allowed", await cam.wait(lambda s: s["look"] == 1), str(cam.status))
        before = cam.status["photos"]
        cam.seen.clear()
        await cam.send("shoot")
        check("shoot in video mode -> recording", await cam.wait(lambda s: s.get("recording")), str(cam.status))
        await cam.send("look 3")
        check("look during clip -> refused 'recording'",
              await cam.wait(lambda s: s["error"] == "recording" and s["recording"]), str(cam.status))
        check("clip recorded frames",
              await cam.wait(lambda s: s["frames"] >= 4 and s["recording"], timeout=15), str(cam.status))
        await cam.send("lapse stop")
        ok = await cam.wait(lambda s: not s["recording"] and s["photos"] == before + 1
                            and s["last"].startswith("IMG_"), timeout=30)
        check("lapse stop -> clip saved, count +1", ok, str(cam.status))
        clip = int(cam.status["last"][4:])
        listed = json.loads(await fetch_file(client, "list") or "[]")
        check("list includes the new clip", any(n == clip for n, _ in listed), str(listed[-3:]))
        check("clip thumb -> refused", await fetch_file(client, f"thumb {clip}") is None)

        await cam.send("mode single")
        for cmd in ("look 0", "interval 1", "mode single"):
            await cam.send(cmd)
        check("restored Normal / 10 s / single",
              await cam.wait(lambda s: s["look"] == 0 and s["interval"] == 1 and s["mode"] == "single"),
              str(cam.status))
    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


asyncio.run(main())
