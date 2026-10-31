"""
Cloud inference server for the covered-face detector.

- Pi streams JPEG frames over a WebSocket:   wss://<your-space>.hf.space/ws?key=API_KEY
- Server runs YOLO on each frame and replies with JSON detections.
- Live viewer page (any browser/phone):     https://<your-space>.hf.space/?key=API_KEY
- Test with a single image:                 POST /detect?key=API_KEY  (multipart field "file")

Settings come from environment variables (Hugging Face: Space Settings -> Variables and secrets):
    API_KEY    secret password the Pi and viewer must send (REQUIRED for privacy)
    MODEL_PATH default best.pt
    IMG_SIZE   default 640
    CONF       default 0.5
"""
import asyncio, os, secrets, threading, time

import cv2
import numpy as np
from fastapi import FastAPI, File, HTTPException, Query, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse, Response
from ultralytics import YOLO

API_KEY = os.getenv("API_KEY", "")
MODEL_PATH = os.getenv("MODEL_PATH", "best.pt")
IMG_SIZE = int(os.getenv("IMG_SIZE", "640"))
CONF = float(os.getenv("CONF", "0.5"))
COVERED_CLASS = 1  # 'covered_face'

if not API_KEY:
    print("WARNING: API_KEY is not set - anyone can use this server and watch the camera!")

model = YOLO(MODEL_PATH, task="detect")
model.predict(np.zeros((IMG_SIZE, IMG_SIZE, 3), np.uint8), imgsz=IMG_SIZE, verbose=False)  # warm-up
print("Model loaded:", MODEL_PATH, "classes:", model.names)

infer_lock = threading.Lock()        # one inference at a time (CPU is small)
latest = {"jpeg": None, "ts": 0.0, "covered": 0, "faces": 0, "fps": 0.0,
          "last_alert": None, "clients": 0, "infer_ms": 0.0}

app = FastAPI(title="Covered Face Detector")


def check_key(key: str):
    if API_KEY and not secrets.compare_digest(key or "", API_KEY):
        raise HTTPException(status_code=401, detail="bad key")


# Phone app, alert list and push notifications (events.py)
try:
    from events import setup_events
    events = setup_events(app, check_key, lambda: latest["jpeg"])
except Exception as e:
    print("Phone app / alerts disabled:", e)
    events = None


def infer(jpeg: bytes):
    frame = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    if frame is None:
        return None
    t0 = time.time()
    with infer_lock:
        r = model.predict(frame, imgsz=IMG_SIZE, conf=CONF, verbose=False)[0]
    ms = (time.time() - t0) * 1000

    dets = []
    for b in r.boxes:
        c = int(b.cls)
        x1, y1, x2, y2 = (int(v) for v in b.xyxy[0].tolist())
        dets.append({"cls": c, "name": model.names[c], "conf": round(float(b.conf), 3),
                     "box": [x1, y1, x2, y2]})
    covered = sum(d["cls"] == COVERED_CLASS for d in dets)

    # keep the newest annotated frame for the viewer page
    ok, enc = cv2.imencode(".jpg", r.plot(), [cv2.IMWRITE_JPEG_QUALITY, 70])
    now = time.time()
    if ok:
        dt = now - latest["ts"] if latest["ts"] else 0
        latest.update(jpeg=enc.tobytes(), ts=now, covered=covered, faces=len(dets),
                      infer_ms=round(ms, 1),
                      fps=round(0.8 * latest["fps"] + 0.2 * (1 / dt), 2) if dt > 0 else latest["fps"])
        if covered:
            latest["last_alert"] = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(now)) + " UTC"
    return {"detections": dets, "covered": covered, "infer_ms": round(ms, 1)}


@app.get("/health")
def health():
    return {"ok": True, "model": MODEL_PATH, "classes": model.names, "imgsz": IMG_SIZE}


@app.websocket("/ws")
async def ws_stream(ws: WebSocket, key: str = Query("")):
    if API_KEY and not secrets.compare_digest(key, API_KEY):
        await ws.close(code=1008)
        return
    await ws.accept()
    latest["clients"] += 1
    print("Pi connected")
    try:
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if msg.get("bytes") is not None:          # camera frame -> detections
                res = await asyncio.to_thread(infer, msg["bytes"])
                await ws.send_json(res or {"error": "bad image"})
            elif msg.get("text") and events:          # alert event from the Pi (no reply)
                events.handle_text(msg["text"])
    except WebSocketDisconnect:
        pass
    finally:
        latest["clients"] -= 1
        print("Pi disconnected")


@app.post("/detect")
async def detect(file: UploadFile = File(...), key: str = Query("")):
    check_key(key)
    res = await asyncio.to_thread(infer, await file.read())
    if res is None:
        raise HTTPException(400, "not an image")
    return res


@app.get("/latest.jpg")
def latest_jpg(key: str = Query("")):
    check_key(key)
    if not latest["jpeg"]:
        raise HTTPException(404, "no frame yet")
    return Response(latest["jpeg"], media_type="image/jpeg",
                    headers={"Cache-Control": "no-store"})


@app.get("/status")
def status(key: str = Query("")):
    check_key(key)
    s = {k: v for k, v in latest.items() if k != "jpeg"}
    s["age_sec"] = round(time.time() - latest["ts"], 1) if latest["ts"] else None
    return JSONResponse(s)


VIEWER = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Covered Face Monitor</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#eee;text-align:center}
h1{font-size:18px;margin:12px}
img{max-width:100%;border-radius:8px;background:#222;min-height:200px}
#bar{padding:10px;font-size:15px}
.alert{background:#c62828;color:#fff;font-weight:700;padding:10px;border-radius:6px;margin:8px 16px}
.ok{background:#2e7d32;color:#fff;padding:10px;border-radius:6px;margin:8px 16px}
.off{background:#555;color:#fff;padding:10px;border-radius:6px;margin:8px 16px}
</style></head><body>
<h1>Covered Face Monitor</h1>
<div id="state" class="off">Waiting for camera...</div>
<img id="cam" alt="camera">
<div id="bar"></div>
<script>
const key = new URLSearchParams(location.search).get("key") || "";
const cam = document.getElementById("cam"), st = document.getElementById("state"), bar = document.getElementById("bar");
let busy = false;
function frame(){ if(busy) return; busy = true;
  const im = new Image();
  im.onload = () => { cam.src = im.src; busy = false; };
  im.onerror = () => { busy = false; };
  im.src = "latest.jpg?key=" + encodeURIComponent(key) + "&t=" + Date.now(); }
async function status(){
  try{
    const r = await fetch("status?key=" + encodeURIComponent(key));
    if(r.status === 401){ st.className="alert"; st.textContent="Wrong or missing ?key="; return; }
    const s = await r.json();
    if(s.age_sec === null || s.age_sec > 5){ st.className="off"; st.textContent="Camera offline"; }
    else if(s.covered > 0){ st.className="alert"; st.textContent="ALERT: " + s.covered + " covered face(s)"; }
    else { st.className="ok"; st.textContent="OK - no covered faces"; }
    bar.textContent = "FPS " + s.fps + " | inference " + s.infer_ms + " ms | last alert: " + (s.last_alert || "none");
  }catch(e){ st.className="off"; st.textContent="Server unreachable"; }
}
setInterval(frame, 300); setInterval(status, 1000); frame(); status();
</script></body></html>"""


@app.get("/", response_class=HTMLResponse)
def viewer():
    return VIEWER
