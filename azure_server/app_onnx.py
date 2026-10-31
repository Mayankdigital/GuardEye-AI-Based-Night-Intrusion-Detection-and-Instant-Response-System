"""
LIGHT cloud server (for 1 GB VMs): same as app.py but uses ONNX Runtime instead of PyTorch.
RAM use about 250-300 MB. Same endpoints, same API key, same Pi client.

    /ws?key=...        Pi streams JPEG frames, gets JSON detections back
    /?key=...          live viewer page
    /detect?key=...    POST one image (test)
    /health            status

Environment variables:
    API_KEY     secret key (required)
    MODEL_PATH  default best.onnx
    CONF        default 0.5
    IOU         default 0.45
    THREADS     default 2
"""
import ast, asyncio, os, secrets, threading, time

import cv2
import numpy as np
import onnxruntime as ort
from fastapi import FastAPI, File, HTTPException, Query, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse, Response

API_KEY = os.getenv("API_KEY", "")
MODEL_PATH = os.getenv("MODEL_PATH", "best.onnx")
CONF = float(os.getenv("CONF", "0.5"))
IOU = float(os.getenv("IOU", "0.45"))
THREADS = int(os.getenv("THREADS", "2"))
COVERED_CLASS = 1  # 'covered_face'

if not API_KEY:
    print("WARNING: API_KEY is not set - anyone can use this server and watch the camera!")

# ---------------- model ----------------
so = ort.SessionOptions()
so.intra_op_num_threads = THREADS
so.inter_op_num_threads = 1
sess = ort.InferenceSession(MODEL_PATH, sess_options=so, providers=["CPUExecutionProvider"])
INPUT = sess.get_inputs()[0]
IN_H, IN_W = int(INPUT.shape[2]), int(INPUT.shape[3])

meta = sess.get_modelmeta().custom_metadata_map
try:
    NAMES = {int(k): v for k, v in ast.literal_eval(meta.get("names", "")).items()}
except Exception:
    NAMES = {0: "face", 1: "covered_face"}
NC = len(NAMES)
print(f"Model loaded: {MODEL_PATH}  input {IN_W}x{IN_H}  classes {NAMES}")


def letterbox(img):
    h, w = img.shape[:2]
    r = min(IN_H / h, IN_W / w)
    nw, nh = int(round(w * r)), int(round(h * r))
    canvas = np.full((IN_H, IN_W, 3), 114, np.uint8)
    top, left = (IN_H - nh) // 2, (IN_W - nw) // 2
    canvas[top:top + nh, left:left + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return canvas, r, left, top


def run_model(frame):
    img, r, dx, dy = letterbox(frame)
    x = np.ascontiguousarray(img[:, :, ::-1].transpose(2, 0, 1))[None].astype(np.float32) / 255.0
    out = sess.run(None, {INPUT.name: x})[0][0]          # (4+nc, anchors) for YOLOv8/11
    if out.shape[0] != 4 + NC and out.shape[1] == 4 + NC:
        out = out.T
    out = out.T                                           # (anchors, 4+nc)

    scores = out[:, 4:4 + NC]
    cls = scores.argmax(1)
    conf = scores[np.arange(len(cls)), cls]
    keep = conf >= CONF
    if not keep.any():
        return []
    b, cls, conf = out[keep, :4], cls[keep], conf[keep]

    # cx,cy,w,h (model input pixels) -> x1,y1,x2,y2 (original frame pixels)
    h0, w0 = frame.shape[:2]
    x1 = np.clip((b[:, 0] - b[:, 2] / 2 - dx) / r, 0, w0)
    y1 = np.clip((b[:, 1] - b[:, 3] / 2 - dy) / r, 0, h0)
    x2 = np.clip((b[:, 0] + b[:, 2] / 2 - dx) / r, 0, w0)
    y2 = np.clip((b[:, 1] + b[:, 3] / 2 - dy) / r, 0, h0)

    # per-class NMS (offset boxes by class so classes don't suppress each other)
    off = cls.astype(np.float32) * 10000
    rects = np.stack([x1 + off, y1, x2 - x1, y2 - y1], 1).tolist()
    idx = cv2.dnn.NMSBoxes(rects, conf.tolist(), CONF, IOU)
    idx = np.array(idx).reshape(-1)

    return [{"cls": int(cls[i]), "name": NAMES.get(int(cls[i]), str(cls[i])),
             "conf": round(float(conf[i]), 3),
             "box": [int(x1[i]), int(y1[i]), int(x2[i]), int(y2[i])]} for i in idx]


def draw(frame, dets):
    out = frame.copy()
    for d in dets:
        x1, y1, x2, y2 = d["box"]
        color = (0, 0, 255) if d["cls"] == COVERED_CLASS else (0, 200, 0)
        cv2.rectangle(out, (x1, y1), (x2, y2), color, 2)
        cv2.putText(out, f'{d["name"]} {d["conf"]:.2f}', (x1, max(15, y1 - 5)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
    return out


# warm-up
run_model(np.zeros((480, 640, 3), np.uint8))

# ---------------- server ----------------
infer_lock = threading.Lock()
latest = {"jpeg": None, "ts": 0.0, "covered": 0, "faces": 0, "fps": 0.0,
          "last_alert": None, "clients": 0, "infer_ms": 0.0}
app = FastAPI(title="Covered Face Detector (ONNX)")


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
        dets = run_model(frame)
    ms = (time.time() - t0) * 1000
    covered = sum(d["cls"] == COVERED_CLASS for d in dets)

    ok, enc = cv2.imencode(".jpg", draw(frame, dets), [cv2.IMWRITE_JPEG_QUALITY, 70])
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
    return {"ok": True, "model": MODEL_PATH, "classes": NAMES, "imgsz": [IN_W, IN_H], "engine": "onnxruntime"}


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
    return Response(latest["jpeg"], media_type="image/jpeg", headers={"Cache-Control": "no-store"})


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
