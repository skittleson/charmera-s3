"""Drive the Web Bluetooth page against the real camera through Chrome's DevTools port.

Chrome on Linux needs Web Bluetooth switched on, so start a throwaway headless one:

    /opt/google/chrome/chrome --headless=new --user-data-dir=/tmp/chrome-bt \
        --remote-debugging-port=9333 --enable-experimental-web-platform-features about:blank &
    python3 -m http.server -d docs 8765 &
    python3 tools/web_test.py [devtools-port] [page-url]

Takes one photo and a short 5 s time-lapse; leaves Normal / single shot / 10 s.
"""
import asyncio, json, sys
import requests, websockets

PORT = sys.argv[1] if len(sys.argv) > 1 else "9333"
URL = sys.argv[2] if len(sys.argv) > 2 else "http://localhost:8765/"
log = []


class Cdp:
    def __init__(self, ws):
        self.ws, self.n, self.pending, self.events = ws, 0, {}, asyncio.Queue()

    async def pump(self):
        async for raw in self.ws:
            m = json.loads(raw)
            if "id" in m and m["id"] in self.pending:
                self.pending.pop(m["id"]).set_result(m)
            elif "method" in m:
                await self.events.put(m)

    async def call(self, method, params=None, session=None):
        self.n += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[self.n] = fut
        msg = {"id": self.n, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        await self.ws.send(json.dumps(msg))
        r = await asyncio.wait_for(fut, 30)
        if "error" in r:
            raise RuntimeError(f"{method}: {r['error']}")
        return r.get("result", {})


async def main():
    global ok
    browser_ws = requests.get(f"http://127.0.0.1:{PORT}/json/version", timeout=3).json()["webSocketDebuggerUrl"]
    async with websockets.connect(browser_ws, max_size=None) as ws:
        cdp = Cdp(ws)
        pump = asyncio.create_task(cdp.pump())
        tid = (await cdp.call("Target.createTarget", {"url": "about:blank"}))["targetId"]
        sid = (await cdp.call("Target.attachToTarget", {"targetId": tid, "flatten": True}))["sessionId"]

        async def ev(expr, gesture=False):
            r = await cdp.call("Runtime.evaluate", {"expression": expr, "awaitPromise": True,
                                                    "returnByValue": True, "userGesture": gesture}, sid)
            return r.get("result", {}).get("value")

        await cdp.call("Page.enable", {}, sid)
        await cdp.call("Runtime.enable", {}, sid)
        await cdp.call("DeviceAccess.enable", {}, sid)
        # Test the files on disk, not a service-worker cached copy of an older build.
        await cdp.call("Network.enable", {}, sid)
        await cdp.call("Network.setBypassServiceWorker", {"bypass": True}, sid)
        await cdp.call("Network.setCacheDisabled", {"cacheDisabled": True}, sid)
        await cdp.call("Page.navigate", {"url": URL}, sid)
        await asyncio.sleep(2)
        print("page:", await ev("document.title"), "| unsupported shown:", await ev("!$('unsupported').hidden"))

        await ev("document.getElementById('connect').click()", gesture=True)
        prompt = None
        end = asyncio.get_event_loop().time() + 20
        while asyncio.get_event_loop().time() < end:
            try:
                m = await asyncio.wait_for(cdp.events.get(), 1)
            except asyncio.TimeoutError:
                continue
            if m["method"] == "DeviceAccess.deviceRequestPrompted":
                prompt = m["params"]
                if prompt["devices"]:
                    break
        if not prompt or not prompt["devices"]:
            print("FAIL no device offered in the picker", prompt)
            return
        print("picker offers:", [d["name"] for d in prompt["devices"]])
        await cdp.call("DeviceAccess.selectPrompt", {"id": prompt["id"], "deviceId": prompt["devices"][0]["id"]}, sid)

        async def until(expr, secs=15):
            for _ in range(secs * 10):
                if await ev(expr):
                    return True
                await asyncio.sleep(0.1)
            return False

        def report(name, ok):
            ok = bool(ok)
            log.append(ok)
            print(("PASS " if ok else "FAIL ") + name)

        report("connected, controls enabled", await until("ready && !$('app').classList.contains('disabled') && status !== null"))
        if not log[-1]:
            print("    hint:", await ev("$('connect-hint').textContent"))
            print("    device connected:", await ev("!!(device && device.gatt.connected)"))
        print("    chip:", await ev("$('conn-chip').textContent"))
        print("    chips:", await ev("['photos','left','battery','camera'].map(i=>$(i).textContent).join(' | ')"))
        report("look select filled from Info (12)", await ev("$('looks').options.length") == 12)
        report("interval list filled (5)", await ev("$('interval').options.length") == 5)

        await ev("$('looks').value='2'; $('looks').dispatchEvent(new Event('change'))")
        report("look select -> camera reports look 2", await until("status.look === 2"))
        await ev("$('looks').value='0'; $('looks').dispatchEvent(new Event('change'))")
        report("look back to Normal", await until("status.look === 0"))

        await ev("$('mode-lapse').click()")
        report("mode Time-lapse -> mode lapse", await until("status.mode === 'lapse' && $('mode-lapse').getAttribute('aria-checked') === 'true'"))
        await ev("$('mode-single').click()")
        report("mode Single shot", await until("status.mode === 'single'"))

        before = await ev("status ? status.photos : -1")
        if before is None or before < 0:
            print("FAIL cannot proceed without status"); ok = False
            await cdp.call("Target.closeTarget", {"targetId": tid}); pump.cancel(); return
        await ev("$('shoot').click()")
        report("shutter -> busy shown, controls locked", await until("status.busy && $('shoot').classList.contains('busy')"))
        report("shutter -> photo saved (Normal, sensor JPEG)", await until(f"status.photos === {before + 1} && !status.busy", 25))
        print("   ", await ev("$('last').textContent"))
        report("auto-preview thumbnail shown", await until("!$('preview').hidden && $('preview').src.startsWith('blob:')", 20))

        await ev("$('interval').value='0'; $('interval').dispatchEvent(new Event('change'))")
        report("interval 5 s", await until("status.interval === 0"))
        await ev("$('lapse-start').click()")
        report("Start -> running line shown, shutter locked, Stop enabled",
               await until("status.running && !$('lapse-line').hidden && $('shoot').disabled && !$('lapse-stop').disabled"))
        report("time-lapse 2 frames", await until("status.frames >= 2", 30))
        print("   ", await ev("$('lapse-line').textContent"))
        await ev("window.confirm = () => true; $('lapse-stop').click()")
        report("Stop (confirmed) -> stopped, controls unlocked", await until("!status.running && !$('shoot').disabled", 20))

        # gallery: tab switch triggers list; rows appear (no thumbnails by design)
        await ev("$('tab-gallery').click()")
        report("gallery lists card photos", await until("gallery.length > 0 && $('photo-list').children.length > 0", 25))
        print("    gallery-info:", await ev("$('gallery-info').textContent"))
        print("    gallery:", await ev("gallery.length"), "photos, first row", await ev("$('photo-list').firstChild.textContent"))
        report("gallery row opens viewer with thumbnail",
               await ev("(async () => { $('photo-list').firstChild.querySelector('.prow-open').click(); "
                        "await new Promise(r => setTimeout(r, 8000)); "
                        "return !$('viewer').hidden && !$('viewer-img').hidden; })()"))

        await ev("$('interval').value='1'; $('interval').dispatchEvent(new Event('change'))")
        await until("status.interval === 1")
        await ev("$('conn-chip').click()")
        report("chip -> controls disabled",
               await until("$('app').classList.contains('disabled')"))
        print(f"\n{sum(log)}/{len(log)} passed")
        ok = all(log)
        await cdp.call("Target.closeTarget", {"targetId": tid})
        pump.cancel()


ok = False
asyncio.run(main())
sys.exit(0 if ok else 1)
