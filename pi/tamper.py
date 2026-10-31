"""
Camera tamper detection (runs on the Raspberry Pi, no internet needed).

How it decides:
  1. Every 0.3 s it measures the camera image:
        brightness = average grey level
        contrast   = spread of grey levels
        sharpness  = amount of edges/detail (Laplacian variance)
  2. During the first LEARN_SEC seconds it learns what "normal" looks like
     (then keeps adjusting slowly while everything is normal).
  3. The image is "degraded" when sharpness OR contrast falls far below normal
     (spray, smear, tape, hand, cloth over the lens).
  4. FINAL RULE:
        IR sensor detects something close to the camera (within IR_WINDOW_SEC before,
        or while the view is blocked)  AND  the view then stays blocked/blurred for more
        than TAMPER_BLOCK_SEC (15 s)  ->  "CAMERA TAMPERING" alert.
        Anything else (lights off, blur without IR, short blocks) -> nothing.

Run alone to see live measurements (good for testing and tuning):
    ./venv/bin/python tamper.py            (Pi Camera)
    ./venv/bin/python tamper.py --usb      (USB webcam)

Settings (environment variables, all optional):
    TAMPER_ENABLED=1        0 = turn tamper detection off
    IR_PIN=22               GPIO number of the IR sensor signal (physical pin 15)
    IR_ACTIVE_LOW=1         1 = sensor output goes LOW when an object is close (LED on)
    IR_WINDOW_SEC=10        "recent" IR activity = within this many seconds
    LEARN_SEC=15            seconds to learn the normal image at start
    SHARP_RATIO=0.35        degraded if sharpness < 35% of normal
    CONTRAST_RATIO=0.45     degraded if contrast  < 45% of normal
    TAMPER_BLOCK_SEC=15     view must stay blocked/blurred longer than this (after IR)
    TAMPER_COOLDOWN_SEC=30  minimum time between tamper alerts
"""
import os, threading, time
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np

IR_PIN = int(os.getenv("IR_PIN", "22"))
IR_ACTIVE_LOW = os.getenv("IR_ACTIVE_LOW", "1") == "1"
IR_WINDOW_SEC = float(os.getenv("IR_WINDOW_SEC", "10"))
LEARN_SEC = float(os.getenv("LEARN_SEC", "15"))
SHARP_RATIO = float(os.getenv("SHARP_RATIO", "0.35"))
CONTRAST_RATIO = float(os.getenv("CONTRAST_RATIO", "0.45"))
BLOCK_SEC = float(os.getenv("TAMPER_BLOCK_SEC", "15"))
COOLDOWN_SEC = float(os.getenv("TAMPER_COOLDOWN_SEC", "30"))
RECOVER_SEC = 3.0
SAMPLE_SEC = 0.3
SNAP_DIR = Path(os.getenv("SNAP_DIR", "alerts"))


def measure(frame):
    """Return (brightness, contrast, sharpness) on a small, slightly blurred grey copy."""
    g = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame.ndim == 3 else frame
    g = cv2.resize(g, (160, 120), interpolation=cv2.INTER_AREA)
    g = cv2.GaussianBlur(g, (3, 3), 0)          # removes camera noise so it isn't counted as detail
    brightness = float(g.mean())
    contrast = float(g.std())
    sharpness = float(cv2.Laplacian(g, cv2.CV_64F).var())
    return brightness, contrast, sharpness


class IRSensor:
    """Remembers the last time the IR sensor saw something close."""

    def __init__(self):
        self.last_seen = 0.0
        self.ok = False
        try:
            from gpiozero import DigitalInputDevice
            # pull_up=True: the sensor can only pull the line LOW (needs the Pi's pull-up)
            self.dev = DigitalInputDevice(IR_PIN, pull_up=True) if IR_ACTIVE_LOW \
                else DigitalInputDevice(IR_PIN, pull_up=False)
            self.dev.when_activated = self._seen
            if self.dev.is_active:
                self._seen()
            self.ok = True
        except Exception as e:
            print(f"[tamper] IR sensor not available ({e}) - using camera only")

    def _seen(self):
        self.last_seen = time.time()

    def active_now(self):
        return self.ok and self.dev.is_active

    def recent(self, window=IR_WINDOW_SEC):
        if self.active_now():
            self.last_seen = time.time()
        return self.ok and (time.time() - self.last_seen) <= window


class TamperDetector:
    """Pure decision logic: feed it frames, it returns events. (No camera, no GPIO.)"""

    def __init__(self, ir=None, now=time.time):
        self.ir = ir
        self.now = now
        self.t_start = None
        self.learn = []
        self.base = None                 # dict brightness/contrast/sharpness
        self.degraded_since = None
        self.ir_during = False
        self.normal_since = None
        self.state = "learning"          # learning | normal | degraded | tamper
        self.last_alert = -1e9
        self.last = None

    def _ir_recent(self):
        return bool(self.ir and self.ir.recent())

    def update(self, frame):
        """Returns None, or an event dict {"type": ..., "msg": ...} when something changes."""
        t = self.now()
        b, c, s = measure(frame)
        self.last = (b, c, s)

        if self.base is None:            # ---- learning the normal view ----
            if self.t_start is None:
                self.t_start = t
            self.learn.append((b, c, s))
            if t - self.t_start >= LEARN_SEC and len(self.learn) >= 10:
                arr = np.array(self.learn)
                med = np.median(arr, 0)
                self.base = {"b": max(med[0], 5), "c": max(med[1], 3), "s": max(med[2], 5)}
                self.state = "normal"
                return {"type": "ready",
                        "msg": f"normal view learned: brightness {med[0]:.0f}, "
                               f"contrast {med[1]:.0f}, sharpness {med[2]:.0f}"}
            return None

        rs = s / self.base["s"]
        rc = c / self.base["c"]
        rb = b / self.base["b"]
        degraded = rs < SHARP_RATIO or rc < CONTRAST_RATIO
        ir_recent = self._ir_recent()

        if not degraded:
            self.degraded_since = None
            self.ir_during = False
            if self.normal_since is None:
                self.normal_since = t
            # slowly follow normal changes (sunlight moving, etc.)
            if self.state == "normal":
                a = 0.01
                self.base["b"] += a * (b - self.base["b"])
                self.base["c"] += a * (c - self.base["c"])
                self.base["s"] += a * (s - self.base["s"])
            elif t - self.normal_since >= RECOVER_SEC:
                old = self.state
                self.state = "normal"
                if old != "degraded":     # brief blur that never became an event -> stay quiet
                    return {"type": "restored", "msg": f"camera view back to normal (was: {old})"}
            return None

        # ---- degraded ----
        self.normal_since = None
        if self.degraded_since is None:
            self.degraded_since = t
            self.ir_during = ir_recent
        self.ir_during = self.ir_during or ir_recent
        dur = t - self.degraded_since

        # FINAL RULE: IR saw something close (just before or during) AND view blocked/blurred
        # for more than BLOCK_SEC -> alert.  Anything else -> nothing.
        if self.state == "normal":
            self.state = "degraded"
        if self.ir_during and dur >= BLOCK_SEC and self.state != "tamper":
            self.state = "tamper"
            if t - self.last_alert < COOLDOWN_SEC:
                return None
            self.last_alert = t
            detail = f"sharpness {rs * 100:.0f}%, contrast {rc * 100:.0f}%, brightness {rb * 100:.0f}% of normal"
            return {"type": "tamper",
                    "msg": f"CAMERA TAMPERING: view blocked/blurred for {dur:.0f}s "
                           f"after an object came close to the camera ({detail})"}
        return None


class TamperMonitor(threading.Thread):
    """Runs the detector in the background using the shared camera, raises alerts."""

    def __init__(self, cam, buzzer=None, notify=None):
        super().__init__(daemon=True)
        self.cam, self.buzzer, self.notify = cam, buzzer, notify
        self.ir = IRSensor()
        self.det = TamperDetector(self.ir)
        SNAP_DIR.mkdir(parents=True, exist_ok=True)

    def run(self):
        print(f"[tamper] learning the normal camera view for {LEARN_SEC:.0f}s - keep the camera clear")
        last_id = -1
        while True:
            time.sleep(SAMPLE_SEC)
            fid, frame = self.cam.get()
            if frame is None or fid == last_id:
                continue
            last_id = fid
            try:
                ev = self.det.update(frame)
            except Exception as e:
                print("[tamper] error:", e)
                continue
            if ev:
                self.handle(ev, frame)

    def handle(self, ev, frame):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[tamper] {ts} {ev['msg']}")
        if ev["type"] not in ("tamper", "possible"):
            return
        path = SNAP_DIR / f"tamper_{datetime.now():%Y%m%d_%H%M%S}.jpg"
        cv2.imwrite(str(path), frame)
        if self.buzzer:
            try:   # long beeps = tamper (short beeps are used for covered faces)
                n = 6 if ev["type"] == "tamper" else 3
                self.buzzer.beep(on_time=0.8, off_time=0.3, n=n, background=True)
            except Exception as e:
                print("[tamper] buzzer error:", e)
        if self.notify:
            threading.Thread(target=self.notify, args=(path, f"{ev['msg']} at {ts}"), daemon=True).start()


# ---------------- run alone: live measurements ----------------
if __name__ == "__main__":
    import argparse
    from stream_client import Camera, make_buzzer, telegram

    ap = argparse.ArgumentParser()
    ap.add_argument("--usb", action="store_true")
    args = ap.parse_args()
    cam = Camera(args.usb)
    cam.start()
    mon = TamperMonitor(cam, make_buzzer(), telegram)
    mon.start()
    while True:
        time.sleep(1)
        d = mon.det
        if d.last is None:
            continue
        b, c, s = d.last
        ir = "CLOSE" if mon.ir.active_now() else ("recent" if mon.ir.recent() else "-")
        if d.base:
            print(f"bright {b:5.0f} ({b / d.base['b'] * 100:3.0f}%) | contrast {c:5.1f} "
                  f"({c / d.base['c'] * 100:3.0f}%) | sharp {s:7.1f} ({s / d.base['s'] * 100:3.0f}%) "
                  f"| IR {ir:6} | state {d.state}")
        else:
            print(f"bright {b:5.0f} | contrast {c:5.1f} | sharp {s:7.1f} | IR {ir:6} | learning...")
