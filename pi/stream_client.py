"""
Raspberry Pi client: streams the camera to the cloud server and raises the alert.

The Pi does NOT run the model. It only:
  1. grabs camera frames (Pi Camera or USB webcam)
  2. sends them as JPEG over a WebSocket to the cloud
  3. reads back the detections
  4. if a covered face is seen in CONFIRM_FRAMES results in a row:
       buzzer beeps + snapshot saved + (optional) Telegram photo

Settings are read from environment variables (see pi/config.env):
    SERVER_URL  e.g. wss://yourname-mask-alert.hf.space
    API_KEY     same secret as on the server
    TG_TOKEN, TG_CHAT   optional Telegram alert

Run:
    python stream_client.py            # Pi Camera
    python stream_client.py --usb      # USB webcam
"""
import argparse, asyncio, json, os, threading, time
from collections import deque
from datetime import datetime
from pathlib import Path
from urllib.parse import quote

import cv2
import websockets

# ---------------- settings ----------------
SERVER_URL = os.getenv("SERVER_URL", "ws://localhost:7860").rstrip("/")
API_KEY = os.getenv("API_KEY", "")
TG_TOKEN = os.getenv("TG_TOKEN", "")
TG_CHAT = os.getenv("TG_CHAT", "")

FRAME_W, FRAME_H = 640, 480
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "70"))  # lower = less internet data, slightly worse detection
MAX_FPS = float(os.getenv("MAX_FPS", "8"))           # upper limit of frames sent per second
MAX_INFLIGHT = 2                  # frames "on the way" at once (hides internet delay)
CONFIRM_FRAMES = int(os.getenv("CONFIRM_FRAMES", "3"))
COOLDOWN_SEC = int(os.getenv("COOLDOWN_SEC", "15"))
COVERED_CLASS = 1
BUZZER_PIN = 17                   # GPIO17 = physical pin 11
SNAP_DIR = Path(os.getenv("SNAP_DIR", "alerts"))
SEND_EVENTS = os.getenv("SEND_EVENTS", "1") == "1"   # send alerts to the server (phone app)
# ------------------------------------------

# Alerts waiting to be sent to the server (kept if the connection is down, sent after reconnect)
EVENT_QUEUE = deque(maxlen=20)
EVENT_LOCK = threading.Lock()


def report_event(kind, msg):
    """kind: 'covered' or 'tamper'. Safe to call from any thread."""
    if SEND_EVENTS:
        with EVENT_LOCK:
            EVENT_QUEUE.append({"type": "event", "kind": kind, "msg": msg})


class Camera(threading.Thread):
    """Reads the camera in the background and always keeps only the newest frame."""

    def __init__(self, use_usb):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.frame, self.frame_id = None, 0
        self.picam, self.cap = None, None
        if not use_usb:
            try:
                from picamera2 import Picamera2
                self.picam = Picamera2()
                self.picam.configure(self.picam.create_video_configuration(
                    main={"size": (FRAME_W, FRAME_H), "format": "RGB888"}))
                self.picam.start()
                print("Camera: Pi Camera")
            except Exception as e:
                print("Pi Camera not available, using USB:", e)
                self.picam = None
        if self.picam is None:
            self.cap = cv2.VideoCapture(0)
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_W)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_H)
            if not self.cap.isOpened():
                raise SystemExit("No camera found")
            print("Camera: USB")

    def run(self):
        while True:
            if self.picam:
                f = self.picam.capture_array()   # "RGB888" here is BGR order, as OpenCV expects
                ok = f is not None
            else:
                ok, f = self.cap.read()
            if ok:
                with self.lock:
                    self.frame, self.frame_id = f, self.frame_id + 1
            else:
                time.sleep(0.05)

    def get(self):
        with self.lock:
            return self.frame_id, self.frame


def make_buzzer():
    try:
        from gpiozero import Buzzer
        return Buzzer(BUZZER_PIN)
    except Exception as e:
        print("Buzzer disabled:", e)
        return None


def telegram(path, text):
    if not (TG_TOKEN and TG_CHAT):
        return
    try:
        import requests
        with open(path, "rb") as f:
            requests.post(f"https://api.telegram.org/bot{TG_TOKEN}/sendPhoto",
                          data={"chat_id": TG_CHAT, "caption": text},
                          files={"photo": f}, timeout=15)
    except Exception as e:
        print("Telegram failed:", e)


def draw(frame, dets):
    out = frame.copy()
    for d in dets:
        x1, y1, x2, y2 = d["box"]
        color = (0, 0, 255) if d["cls"] == COVERED_CLASS else (0, 200, 0)
        cv2.rectangle(out, (x1, y1), (x2, y2), color, 2)
        cv2.putText(out, f'{d["name"]} {d["conf"]:.2f}', (x1, max(15, y1 - 5)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
    return out


class Alerter:
    def __init__(self):
        self.buzzer = make_buzzer()
        self.streak, self.last = 0, 0.0
        SNAP_DIR.mkdir(parents=True, exist_ok=True)

    async def update(self, frame, res):
        dets = res.get("detections", [])
        covered = sum(d["cls"] == COVERED_CLASS for d in dets)
        self.streak = self.streak + 1 if covered else 0
        now = time.time()
        if self.streak >= CONFIRM_FRAMES and now - self.last > COOLDOWN_SEC:
            self.last = now
            ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            path = SNAP_DIR / f"alert_{datetime.now():%Y%m%d_%H%M%S}.jpg"
            cv2.imwrite(str(path), draw(frame, dets))
            msg = f"ALERT: {covered} covered face(s) detected at {ts}"
            print(msg)
            if self.buzzer:
                self.buzzer.beep(on_time=0.2, off_time=0.2, n=5, background=True)
            report_event("covered", f"{covered} covered face(s) detected at {ts}")
            asyncio.get_running_loop().run_in_executor(None, telegram, path, msg)


async def stream(cam, alerter):
    url = f"{SERVER_URL}/ws?key={quote(API_KEY)}"
    async with websockets.connect(url, max_size=None, ping_interval=20, ping_timeout=60,
                                  open_timeout=60) as ws:
        print("Connected to", SERVER_URL)
        slots = asyncio.Semaphore(MAX_INFLIGHT)
        sent = deque()                  # frames waiting for an answer (answers come in order)
        stats = {"n": 0, "t0": time.time(), "lat": 0.0}

        async def sender():
            last_id, min_gap = 0, 1.0 / MAX_FPS
            while True:
                await slots.acquire()
                t_start = time.time()
                while True:              # wait for a NEW camera frame
                    fid, frame = cam.get()
                    if frame is not None and fid != last_id:
                        break
                    await asyncio.sleep(0.01)
                last_id = fid
                while True:              # send waiting alert events first (text, no reply)
                    with EVENT_LOCK:
                        if not EVENT_QUEUE:
                            break
                        ev = EVENT_QUEUE[0]
                    await ws.send(json.dumps(ev))
                    with EVENT_LOCK:
                        if EVENT_QUEUE and EVENT_QUEUE[0] is ev:
                            EVENT_QUEUE.popleft()
                ok, jpg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
                sent.append((time.time(), frame))
                await ws.send(jpg.tobytes())
                wait = min_gap - (time.time() - t_start)
                if wait > 0:
                    await asyncio.sleep(wait)

        async def receiver():
            async for msg in ws:
                slots.release()
                t_sent, frame = sent.popleft()
                res = json.loads(msg)
                if "error" in res:
                    continue
                await alerter.update(frame, res)
                stats["n"] += 1
                stats["lat"] = 0.8 * stats["lat"] + 0.2 * (time.time() - t_sent)
                el = time.time() - stats["t0"]
                if el >= 5:
                    print(f"{stats['n'] / el:.1f} FPS | delay {stats['lat'] * 1000:.0f} ms | "
                          f"server {res.get('infer_ms', 0):.0f} ms | covered now: {res.get('covered', 0)}")
                    stats["n"], stats["t0"] = 0, time.time()

        tasks = [asyncio.create_task(sender()), asyncio.create_task(receiver())]
        done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for t in pending:
            t.cancel()
        for t in done:
            if t.exception():
                raise t.exception()
        raise ConnectionError("server closed the connection")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usb", action="store_true", help="use USB webcam instead of Pi Camera")
    args = ap.parse_args()

    if not API_KEY:
        print("WARNING: API_KEY is empty")
    cam = Camera(args.usb)
    cam.start()
    alerter = Alerter()

    # Camera tamper detection (tamper.py) - runs on the Pi, works even without internet.
    # Turn off with TAMPER_ENABLED=0 in the config file.
    if os.getenv("TAMPER_ENABLED", "1") == "1":
        try:
            from tamper import TamperMonitor
            def notify_tamper(path, text):
                report_event("tamper", text)
                telegram(path, text)
            TamperMonitor(cam, alerter.buzzer, notify_tamper).start()
        except Exception as e:
            print("Tamper detection disabled:", e)

    delay = 2
    while True:                         # reconnect forever (internet drops, Space waking up...)
        t_try = time.time()
        try:
            await stream(cam, alerter)
        except Exception as e:
            if time.time() - t_try > 60:  # it was working for a while -> retry quickly
                delay = 2
            print(f"Connection problem: {e!r} - retrying in {delay}s")
            await asyncio.sleep(delay)
            delay = min(delay * 2, 60)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Stopped")
