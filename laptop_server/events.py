"""
Alerts + phone app (installable web app / PWA) + push notifications.

Used by both servers (app.py on the laptop, app_onnx.py on Azure):
    from events import setup_events
    events = setup_events(app, check_key, lambda: latest["jpeg"])
    ...in the websocket loop, for text messages:  events.handle_text(text)

The Pi sends text messages over its websocket:
    {"type": "event", "kind": "covered" | "tamper", "msg": "..."}

Phone app:            https://<server>/app?key=API_KEY   (then "Install app" / "Add to Home screen")
Push notifications:   need HTTPS (Azure) + `pip install pywebpush`. Without them the app still
                      shows live video and the alert list, just no notifications when closed.
"""
import base64, json, os, threading, time
from collections import deque
from pathlib import Path

from fastapi import HTTPException, Query, Request
from fastapi.responses import HTMLResponse, JSONResponse, Response

DATA_DIR = Path(os.getenv("DATA_DIR", "."))
SUBS_FILE = DATA_DIR / "push_subscriptions.json"
VAPID_FILE = DATA_DIR / "vapid_private.pem"
VAPID_SUB = os.getenv("VAPID_SUB", "mailto:alerts@example.com")
API_KEY = os.getenv("API_KEY", "")
MAX_EVENTS = 100

TITLES = {"covered": "SecureHome: covered face detected", "tamper": "SecureHome: camera tampering!",
          "test": "SecureHome: test notification"}


def _make_icons():
    """Draw the SecureHome app icons (gradient + shield) if they don't exist yet."""
    import cv2
    import numpy as np
    for size in (192, 512):
        p = DATA_DIR / f"sh-icon-{size}.png"
        if p.exists():
            continue
        s = size
        yy, xx = np.mgrid[0:s, 0:s].astype(np.float32)
        t = ((xx + yy) / (2 * s))[..., None]
        a = np.array([241, 102, 99], np.float32)   # indigo (BGR)
        b = np.array([238, 211, 34], np.float32)   # cyan   (BGR)
        img = (a * (1 - t) + b * t).astype(np.uint8)
        k = s / 100.0
        shield = np.array([[50, 18], [76, 27], [76, 48], [72, 62], [62, 74], [50, 82],
                           [38, 74], [28, 62], [24, 48], [24, 27]], np.float32) * k
        cv2.fillPoly(img, [shield.astype(np.int32)], (255, 255, 255), cv2.LINE_AA)
        check = np.array([[38, 49], [47, 58], [63, 40]], np.float32) * k
        cv2.polylines(img, [check.astype(np.int32)], False, (200, 110, 80), max(2, int(7 * k)), cv2.LINE_AA)
        cv2.imwrite(str(p), img)


class Events:
    def __init__(self, check_key, get_jpeg):
        self.check_key = check_key
        self.get_jpeg = get_jpeg
        self.items = deque(maxlen=MAX_EVENTS)
        self.images = {}
        self.next_id = 1
        self.lock = threading.Lock()
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        try:
            self.subs = json.loads(SUBS_FILE.read_text()) if SUBS_FILE.exists() else []
        except Exception:
            self.subs = []
        self.push_ok, self.public_key = self._init_push()
        try:
            _make_icons()
        except Exception as e:
            print("Icons not created:", e)

    # ---------- push notifications ----------
    def _init_push(self):
        try:
            import pywebpush  # noqa: F401
            from py_vapid import Vapid02 as Vapid
            from cryptography.hazmat.primitives import serialization
            if VAPID_FILE.exists():
                v = Vapid.from_file(str(VAPID_FILE))
            else:
                v = Vapid()
                v.generate_keys()
                v.save_key(str(VAPID_FILE))
            raw = v.public_key.public_bytes(serialization.Encoding.X962,
                                            serialization.PublicFormat.UncompressedPoint)
            print(f"Push notifications enabled ({len(self.subs)} phone(s) subscribed)")
            return True, base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
        except Exception as e:
            print("Push notifications disabled:", e)
            return False, None

    def _save_subs(self):
        try:
            SUBS_FILE.write_text(json.dumps(self.subs))
        except Exception as e:
            print("Could not save subscriptions:", e)

    def subscribe(self, sub):
        if not isinstance(sub, dict) or "endpoint" not in sub or "keys" not in sub:
            raise HTTPException(400, "bad subscription")
        with self.lock:
            self.subs = [s for s in self.subs if s.get("endpoint") != sub["endpoint"]] + [sub]
            self._save_subs()
        return len(self.subs)

    def _push_all(self, payload):
        if not self.push_ok or not self.subs:
            return
        from pywebpush import webpush, WebPushException
        dead = []
        for sub in list(self.subs):
            try:
                webpush(subscription_info=sub, data=json.dumps(payload),
                        vapid_private_key=str(VAPID_FILE),
                        vapid_claims={"sub": VAPID_SUB},
                        ttl=3600, headers={"Urgency": "high"}, timeout=10)
            except WebPushException as e:
                code = getattr(getattr(e, "response", None), "status_code", None)
                if code in (404, 410):          # phone unsubscribed / app removed
                    dead.append(sub.get("endpoint"))
                print("Push failed:", code, str(e)[:120])
            except Exception as e:
                print("Push failed:", str(e)[:120])
        if dead:
            with self.lock:
                self.subs = [s for s in self.subs if s.get("endpoint") not in dead]
                self._save_subs()

    # ---------- events ----------
    def add(self, kind, msg):
        jpeg = self.get_jpeg()
        with self.lock:
            eid = self.next_id
            self.next_id += 1
            ev = {"id": eid, "kind": kind, "msg": msg[:300], "ts": time.time(), "image": bool(jpeg)}
            self.items.append(ev)
            if jpeg:
                self.images[eid] = jpeg
            live_ids = {e["id"] for e in self.items}
            for old in [i for i in self.images if i not in live_ids]:
                del self.images[old]
        print(f"EVENT {kind}: {msg}")
        payload = {"id": eid, "kind": kind, "title": TITLES.get(kind, "Alert"), "body": msg[:180],
                   "image": f"/event/{eid}.jpg?key={API_KEY}" if jpeg else None}
        threading.Thread(target=self._push_all, args=(payload,), daemon=True).start()
        return ev

    def handle_text(self, text):
        """Text message from the Pi websocket. Never replies (the Pi pairs replies with frames)."""
        try:
            d = json.loads(text)
        except Exception:
            return
        if d.get("type") == "event" and d.get("kind") in ("covered", "tamper"):
            self.add(d["kind"], str(d.get("msg", "")))


def setup_events(app, check_key, get_jpeg):
    ev = Events(check_key, get_jpeg)

    @app.get("/events")
    def list_events(key: str = Query(""), after: int = 0):
        check_key(key)
        with ev.lock:
            items = [e for e in ev.items if e["id"] > after]
        return {"events": items, "push": ev.push_ok}

    @app.get("/event/{eid}.jpg")
    def event_image(eid: int, key: str = Query("")):
        check_key(key)
        jpg = ev.images.get(eid)
        if not jpg:
            raise HTTPException(404, "no image")
        return Response(jpg, media_type="image/jpeg", headers={"Cache-Control": "private, max-age=86400"})

    @app.get("/push/key")
    def push_key(key: str = Query("")):
        check_key(key)
        return {"enabled": ev.push_ok, "publicKey": ev.public_key, "subscribed": len(ev.subs)}

    @app.post("/push/subscribe")
    async def push_subscribe(request: Request, key: str = Query("")):
        check_key(key)
        n = ev.subscribe(await request.json())
        return {"ok": True, "phones": n}

    @app.post("/push/test")
    def push_test(key: str = Query("")):
        check_key(key)
        ev.add("test", "If you can read this, notifications work.")
        return {"ok": True, "push": ev.push_ok, "phones": len(ev.subs)}

    @app.get("/manifest.json")
    def manifest():
        return JSONResponse({
            "name": "SecureHome", "short_name": "SecureHome", "id": "/app",
            "start_url": "/app", "scope": "/", "display": "standalone",
            "background_color": "#0b1020", "theme_color": "#0b1020",
            "description": "Live camera, covered-face and tampering alerts",
            "icons": [{"src": "/sh-icon-192.png", "sizes": "192x192", "type": "image/png", "purpose": "any"},
                      {"src": "/sh-icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any"},
                      {"src": "/sh-icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable"}],
        }, media_type="application/manifest+json")

    @app.get("/sh-icon-{size}.png")
    def icon(size: int):
        p = DATA_DIR / f"sh-icon-{size}.png"
        if size not in (192, 512) or not p.exists():
            raise HTTPException(404)
        return Response(p.read_bytes(), media_type="image/png",
                        headers={"Cache-Control": "public, max-age=604800"})

    @app.get("/sw.js")
    def service_worker():
        return Response(SW_JS, media_type="application/javascript",
                        headers={"Cache-Control": "no-cache", "Service-Worker-Allowed": "/"})

    @app.get("/app", response_class=HTMLResponse)
    def phone_app():
        return APP_HTML

    @app.get("/.well-known/assetlinks.json")
    def assetlinks():
        # Put the assetlinks.json from PWABuilder next to the server files so the APK
        # opens full screen (without the browser address bar).
        p = DATA_DIR / "assetlinks.json"
        if not p.exists():
            raise HTTPException(404)
        return Response(p.read_bytes(), media_type="application/json")

    return ev


SW_JS = r"""
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
self.addEventListener('push', e => {
  let d = {};
  try { d = e.data.json(); } catch (_) { d = {title: 'SecureHome', body: e.data ? e.data.text() : ''}; }
  const opts = {
    body: d.body || '', tag: 'ev' + (d.id || Date.now()), renotify: true,
    requireInteraction: d.kind === 'tamper', vibrate: d.kind === 'tamper' ? [600, 200, 600, 200, 600] : [300, 100, 300],
    icon: '/sh-icon-192.png', badge: '/sh-icon-192.png', data: {url: '/app#alerts'}
  };
  if (d.image) opts.image = d.image;
  e.waitUntil(self.registration.showNotification(d.title || 'SecureHome', opts));
});
self.addEventListener('notificationclick', e => {
  e.notification.close();
  e.waitUntil(clients.matchAll({type: 'window', includeUncontrolled: true}).then(ws => {
    for (const w of ws) { if (w.url.includes('/app')) { w.navigate('/app#alerts'); return w.focus(); } }
    return clients.openWindow('/app#alerts');
  }));
});
"""

APP_HTML = r"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1020">
<link rel="manifest" href="/manifest.json">
<link rel="icon" href="/sh-icon-192.png"><link rel="apple-touch-icon" href="/sh-icon-192.png">
<title>SecureHome</title>
<style>
:root{
  --bg:#0b1020;--surface:rgba(255,255,255,.045);--surface2:rgba(255,255,255,.07);--line:rgba(255,255,255,.08);
  --text:#eef1f8;--muted:#8d97b5;--accent:#6366f1;--accent2:#22d3ee;
  --red:#f43f5e;--amber:#f59e0b;--green:#10b981;--blue:#60a5fa;
  --safe-b:env(safe-area-inset-bottom,0px);--safe-t:env(safe-area-inset-top,0px)
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;min-height:100%;color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  background:radial-gradient(1200px 600px at 10% -10%,rgba(99,102,241,.22),transparent 60%),
             radial-gradient(900px 500px at 110% 10%,rgba(34,211,238,.12),transparent 60%),var(--bg);background-attachment:fixed}
.hide{display:none!important}
svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;flex:none}

/* header */
header{position:sticky;top:0;z-index:10;padding:calc(14px + var(--safe-t)) 18px 12px;display:flex;align-items:center;gap:12px;
  background:rgba(11,16,32,.72);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border-bottom:1px solid var(--line)}
.logo{width:40px;height:40px;border-radius:12px;display:grid;place-items:center;
  background:linear-gradient(135deg,var(--accent),var(--accent2));box-shadow:0 6px 20px rgba(99,102,241,.45)}
.logo svg{color:#fff;width:22px;height:22px}
.brand{flex:1;min-width:0}.brand b{display:block;font-size:18px;letter-spacing:.2px}.brand span{font-size:12px;color:var(--muted)}
.pill{display:inline-flex;align-items:center;gap:6px;padding:6px 11px;border-radius:999px;font-size:12px;font-weight:700;
  background:var(--surface2);border:1px solid var(--line);color:var(--muted);white-space:nowrap}
.pill i{width:8px;height:8px;border-radius:50%;background:currentColor}
.pill.ok{color:#6ee7b7;background:rgba(16,185,129,.12);border-color:rgba(16,185,129,.3)}
.pill.bad{color:#fda4af;background:rgba(244,63,94,.14);border-color:rgba(244,63,94,.35)}
.pill.ok i,.pill.bad i{animation:pulse 1.6s infinite}
@keyframes pulse{0%{box-shadow:0 0 0 0 currentColor}70%{box-shadow:0 0 0 6px transparent}100%{box-shadow:0 0 0 0 transparent}}

main{padding:16px 16px calc(96px + var(--safe-b));max-width:720px;margin:0 auto}
h2{font-size:20px;margin:6px 2px 14px;display:flex;align-items:center;gap:10px}
h2 small{font-size:12px;color:var(--muted);font-weight:600;background:var(--surface2);padding:3px 9px;border-radius:999px}

/* live */
.video{position:relative;border-radius:22px;overflow:hidden;background:#05070f;aspect-ratio:4/3;border:1px solid var(--line);
  box-shadow:0 18px 40px rgba(0,0,0,.45)}
.video img{width:100%;height:100%;object-fit:cover;display:block}
.video .empty{position:absolute;inset:0;display:grid;place-items:center;text-align:center;color:var(--muted);font-size:14px}
.video .empty svg{width:40px;height:40px;margin-bottom:8px;opacity:.6}
.live-badge{position:absolute;top:12px;left:12px;display:flex;align-items:center;gap:6px;padding:5px 10px;border-radius:8px;
  background:rgba(0,0,0,.55);backdrop-filter:blur(6px);font-size:11px;font-weight:800;letter-spacing:1px}
.live-badge i{width:8px;height:8px;border-radius:50%;background:var(--red);animation:pulse 1.4s infinite;color:var(--red)}
.status-chip{position:absolute;top:12px;right:12px;padding:5px 10px;border-radius:8px;font-size:12px;font-weight:700;
  background:rgba(0,0,0,.55);backdrop-filter:blur(6px)}
.status-chip.ok{color:#6ee7b7}.status-chip.bad{color:#fff;background:rgba(225,29,72,.9)}
.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:14px}
.tile{background:var(--surface);border:1px solid var(--line);border-radius:16px;padding:12px}
.tile span{display:block;font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.6px}
.tile b{display:block;font-size:17px;margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}

/* alerts */
.card{display:flex;gap:12px;align-items:center;background:var(--surface);border:1px solid var(--line);border-radius:18px;
  padding:12px;margin-bottom:10px;animation:in .35s ease}
@keyframes in{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.ic{width:42px;height:42px;border-radius:13px;display:grid;place-items:center;flex:none}
.ic.covered{background:rgba(244,63,94,.15);color:#fb7185}.ic.tamper{background:rgba(245,158,11,.15);color:#fbbf24}
.ic.test{background:rgba(96,165,250,.15);color:#93c5fd}
.card .body{flex:1;min-width:0}.card .ttl{font-weight:700}.card .when{font-size:12px;color:var(--muted)}
.card .msg{font-size:13px;color:#c3cae0;margin-top:2px;overflow:hidden;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical}
.card img{width:74px;height:56px;border-radius:10px;object-fit:cover;flex:none;background:#000}
.none{text-align:center;color:var(--muted);padding:50px 10px}
.none .ic{margin:0 auto 12px;width:64px;height:64px;border-radius:20px;background:rgba(16,185,129,.12);color:#6ee7b7}
.none .ic svg{width:30px;height:30px}

/* settings */
.group{background:var(--surface);border:1px solid var(--line);border-radius:18px;padding:16px;margin-bottom:12px}
.group .row{display:flex;align-items:center;gap:12px}
.group .row .ic{background:rgba(99,102,241,.15);color:#a5b4fc}
.group b{display:block}.group p{margin:2px 0 0;font-size:13px;color:var(--muted)}
.btn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;padding:13px;border-radius:14px;border:0;margin-top:12px;
  font:inherit;font-weight:700;color:#fff;background:linear-gradient(135deg,var(--accent),#4f46e5);box-shadow:0 8px 20px rgba(99,102,241,.35)}
.btn.sec{background:var(--surface2);box-shadow:none;border:1px solid var(--line)}
.btn:active{transform:scale(.98)}
input{width:100%;margin-top:12px;padding:13px;border-radius:14px;border:1px solid var(--line);background:rgba(0,0,0,.25);color:var(--text);font:inherit}

/* bottom nav */
nav{position:fixed;left:0;right:0;bottom:0;z-index:10;display:flex;padding:8px 10px calc(8px + var(--safe-b));
  background:rgba(11,16,32,.85);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border-top:1px solid var(--line)}
nav button{flex:1;position:relative;display:flex;flex-direction:column;align-items:center;gap:3px;padding:6px 0;border:0;background:none;
  color:var(--muted);font:600 11px system-ui,sans-serif}
nav button.sel{color:var(--text)}
nav button.sel .nb{background:linear-gradient(135deg,rgba(99,102,241,.35),rgba(34,211,238,.2))}
.nb{padding:5px 18px;border-radius:999px;display:grid;place-items:center}
.badge{position:absolute;top:0;left:calc(50% + 10px);min-width:18px;height:18px;padding:0 5px;border-radius:9px;background:var(--red);
  color:#fff;font-size:11px;display:grid;place-items:center}

/* toast */
#toasts{position:fixed;top:calc(10px + var(--safe-t));left:12px;right:12px;z-index:50;display:flex;flex-direction:column;gap:8px;
  pointer-events:none;max-width:560px;margin:0 auto}
.toast{pointer-events:auto;display:flex;align-items:center;gap:12px;padding:12px;border-radius:18px;overflow:hidden;position:relative;
  background:rgba(20,26,48,.92);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);border:1px solid var(--line);
  box-shadow:0 16px 40px rgba(0,0,0,.5);animation:drop .45s cubic-bezier(.2,1.2,.4,1)}
.toast.covered{border-color:rgba(244,63,94,.5)}.toast.tamper{border-color:rgba(245,158,11,.55)}
.toast .body{flex:1;min-width:0}.toast b{display:block}.toast span{font-size:13px;color:#c3cae0}
.toast img{width:56px;height:42px;border-radius:9px;object-fit:cover}
.toast .bar{position:absolute;left:0;bottom:0;height:3px;width:100%;transform-origin:left;animation:shrink linear forwards}
.toast.covered .bar{background:var(--red)}.toast.tamper .bar{background:var(--amber)}.toast.test .bar{background:var(--blue)}
.toast.out{animation:up .35s ease forwards}
@keyframes drop{from{opacity:0;transform:translateY(-120%)}to{opacity:1;transform:none}}
@keyframes up{to{opacity:0;transform:translateY(-120%)}}
@keyframes shrink{to{transform:scaleX(0)}}

#full{position:fixed;inset:0;background:rgba(0,0,0,.92);z-index:60;display:none;align-items:center;justify-content:center;padding:16px}
#full img{max-width:100%;max-height:100%;border-radius:12px}
</style></head><body>
<svg style="display:none" xmlns="http://www.w3.org/2000/svg">
  <symbol id="i-shield" viewBox="0 0 24 24"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/><path d="m9 12 2 2 4-4"/></symbol>
  <symbol id="i-cam" viewBox="0 0 24 24"><path d="m22 8-6 4 6 4V8z"/><rect x="2" y="6" width="14" height="12" rx="2"/></symbol>
  <symbol id="i-bell" viewBox="0 0 24 24"><path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/><path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"/></symbol>
  <symbol id="i-gear" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></symbol>
  <symbol id="i-mask" viewBox="0 0 24 24"><circle cx="12" cy="8" r="5"/><path d="M5 13c2 1 4 1.5 7 1.5s5-.5 7-1.5v3c0 2.5-3 5-7 5s-7-2.5-7-5v-3z"/></symbol>
  <symbol id="i-warn" viewBox="0 0 24 24"><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4"/><path d="M12 17h.01"/></symbol>
  <symbol id="i-key" viewBox="0 0 24 24"><circle cx="7.5" cy="15.5" r="5.5"/><path d="m21 2-9.6 9.6"/><path d="m15.5 7.5 3 3L22 7l-3-3"/></symbol>
  <symbol id="i-phone" viewBox="0 0 24 24"><rect x="5" y="2" width="14" height="20" rx="2"/><path d="M12 18h.01"/></symbol>
  <symbol id="i-check" viewBox="0 0 24 24"><path d="M20 6 9 17l-5-5"/></symbol>
</svg>

<header>
  <div class="logo"><svg><use href="#i-shield"/></svg></div>
  <div class="brand"><b>SecureHome</b><span>Smart camera security</span></div>
  <span id="conn" class="pill"><i></i><em id="connt" style="font-style:normal">Connecting</em></span>
</header>

<div id="toasts"></div>

<main>
  <section id="p-live">
    <div class="video">
      <img id="cam" alt="">
      <div id="ov" class="empty"><div><svg><use href="#i-cam"/></svg><div>Waiting for camera...</div></div></div>
      <div class="live-badge"><i></i>LIVE</div>
      <div id="chip" class="status-chip hide"></div>
    </div>
    <div class="tiles">
      <div class="tile"><span>Speed</span><b id="s1">-</b></div>
      <div class="tile"><span>Detection</span><b id="s2">-</b></div>
      <div class="tile"><span>Last alert</span><b id="s3">None</b></div>
    </div>
  </section>

  <section id="p-alerts" class="hide">
    <h2>Alerts <small id="acount">0</small></h2>
    <div id="list"><div class="none"><div class="ic"><svg><use href="#i-check"/></svg></div><b>All quiet</b><div>No alerts so far</div></div></div>
  </section>

  <section id="p-set" class="hide">
    <h2>Settings</h2>
    <div class="group" id="keybox">
      <div class="row"><div class="ic"><svg><use href="#i-key"/></svg></div><div><b>Server key</b><p id="keyst">Enter the key once - it is remembered.</p></div></div>
      <input id="keyin" placeholder="mk-..." autocomplete="off" autocapitalize="off" spellcheck="false">
      <button class="btn" id="keysave">Save key</button>
    </div>
    <div class="group">
      <div class="row"><div class="ic"><svg><use href="#i-bell"/></svg></div><div><b>Notifications</b><p id="nstate">Checking...</p></div></div>
      <button class="btn" id="notif">Turn on notifications</button>
      <button class="btn sec" id="test">Send test notification</button>
    </div>
    <div class="group">
      <div class="row"><div class="ic"><svg><use href="#i-phone"/></svg></div><div><b>Install app</b><p>Chrome menu &#8942; &rarr; Add to Home screen</p></div></div>
      <button class="btn sec hide" id="install">Install SecureHome</button>
    </div>
  </section>
</main>

<nav>
  <button id="t-live" class="sel"><span class="nb"><svg><use href="#i-cam"/></svg></span>Live</button>
  <button id="t-alerts"><span class="nb"><svg><use href="#i-bell"/></svg></span>Alerts<span id="badge" class="badge hide">0</span></button>
  <button id="t-set"><span class="nb"><svg><use href="#i-gear"/></svg></span>Settings</button>
</nav>
<div id="full"><img id="fullimg" alt=""></div>

<script>
const $ = id => document.getElementById(id);
let key = new URLSearchParams(location.search).get('key');
try { if (key) localStorage.setItem('mk', key); else key = localStorage.getItem('mk'); } catch (e) {}
if (location.search) history.replaceState(null, '', '/app' + location.hash);
const q = () => 'key=' + encodeURIComponent(key || '');
const ICON = {covered: 'i-mask', tamper: 'i-warn', test: 'i-bell'};
const LABEL = {covered: 'Covered face detected', tamper: 'Camera tampering', test: 'Test notification'};
const esc = s => String(s).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));

// ---- tabs ----
const tabs = ['live', 'alerts', 'set'];
let unseen = 0;
function show(t) {
  tabs.forEach(x => { $('p-' + x).classList.toggle('hide', x !== t); $('t-' + x).classList.toggle('sel', x === t); });
  if (t === 'alerts') { unseen = 0; $('badge').classList.add('hide'); }
  history.replaceState(null, '', '/app#' + t); window.scrollTo(0, 0);
}
tabs.forEach(t => $('t-' + t).onclick = () => show(t));
if (location.hash === '#alerts') show('alerts'); else if (location.hash === '#set') show('set');
if (key) { $('keyst').textContent = 'Key saved on this phone.'; $('keyin').placeholder = 'Saved - enter a new key to change'; }
else show('set');
$('keysave').onclick = () => { const v = $('keyin').value.trim(); if (!v) return; key = v; try { localStorage.setItem('mk', key); } catch (e) {} location.replace('/app'); };

// ---- live video ----
let busy = false;
function frame() {
  if (busy || !key || document.hidden || $('p-live').classList.contains('hide')) return;
  busy = true;
  const im = new Image();
  im.onload = () => { $('cam').src = im.src; $('ov').classList.add('hide'); busy = false; };
  im.onerror = () => { busy = false; };
  im.src = '/latest.jpg?' + q() + '&t=' + Date.now();
}
setInterval(frame, 350);

// ---- status ----
function conn(cls, txt) { $('conn').className = 'pill ' + cls; $('connt').textContent = txt; }
async function status() {
  if (!key) { conn('', 'No key'); return; }
  try {
    const r = await fetch('/status?' + q());
    if (r.status === 401) { conn('bad', 'Wrong key'); $('keyst').textContent = 'Wrong key - enter it again.'; return; }
    const s = await r.json();
    $('s1').textContent = s.fps + ' fps'; $('s2').textContent = Math.round(s.infer_ms) + ' ms';
    const off = s.age_sec === null || s.age_sec > 6;
    if (off) { conn('', 'Offline'); $('chip').classList.add('hide'); $('ov').classList.remove('hide'); return; }
    conn('ok', 'Live');
    const chip = $('chip'); chip.classList.remove('hide');
    if (s.covered > 0) { chip.className = 'status-chip bad'; chip.textContent = s.covered + ' covered face' + (s.covered > 1 ? 's' : ''); }
    else { chip.className = 'status-chip ok'; chip.textContent = 'All clear'; }
  } catch (e) { conn('', 'No connection'); }
}

// ---- alerts ----
let lastId = 0, first = true, total = 0;
function ago(ts) { const d = Date.now() / 1000 - ts;
  if (d < 60) return 'just now'; if (d < 3600) return Math.floor(d / 60) + ' min ago';
  if (d < 86400) return Math.floor(d / 3600) + ' h ago'; return new Date(ts * 1000).toLocaleDateString(); }
function refreshTimes() { document.querySelectorAll('[data-ts]').forEach(el => el.textContent = ago(+el.dataset.ts) + ' · ' + new Date(+el.dataset.ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'})); }
setInterval(refreshTimes, 30000);
async function events() {
  if (!key) return;
  try {
    const r = await fetch('/events?' + q() + '&after=' + lastId); if (!r.ok) return;
    const d = await r.json();
    if (d.events.length) {
      if ($('list').querySelector('.none')) $('list').innerHTML = '';
      for (const e of d.events) {
        lastId = Math.max(lastId, e.id); total++;
        const img = e.image ? '/event/' + e.id + '.jpg?' + q() : null;
        const c = document.createElement('div'); c.className = 'card';
        c.innerHTML = '<div class="ic ' + e.kind + '"><svg><use href="#' + (ICON[e.kind] || 'i-bell') + '"/></svg></div>' +
          '<div class="body"><div class="ttl">' + (LABEL[e.kind] || esc(e.kind)) + '</div><div class="when" data-ts="' + e.ts + '"></div>' +
          '<div class="msg">' + esc(e.msg) + '</div></div>' + (img ? '<img loading="lazy" src="' + img + '">' : '');
        const im = c.querySelector('img'); if (im) im.onclick = () => { $('fullimg').src = im.src; $('full').style.display = 'flex'; };
        $('list').prepend(c);
        if (!first) toast(e, img);
      }
      refreshTimes();
      $('acount').textContent = total;
      const le = d.events[d.events.length - 1];
      $('s3').textContent = new Date(le.ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
      if (!first && $('p-alerts').classList.contains('hide')) { unseen += d.events.length; $('badge').textContent = unseen; $('badge').classList.remove('hide'); }
    }
    first = false;
  } catch (e) {}
}
$('full').onclick = () => $('full').style.display = 'none';

// ---- pop-up alert at the top (disappears after a few seconds) ----
const TOAST_MS = 5000;
function toast(e, img) {
  const t = document.createElement('div'); t.className = 'toast ' + e.kind;
  t.innerHTML = '<div class="ic ' + e.kind + '"><svg><use href="#' + (ICON[e.kind] || 'i-bell') + '"/></svg></div>' +
    '<div class="body"><b>' + (LABEL[e.kind] || esc(e.kind)) + '</b><span>' + esc(e.msg).slice(0, 90) + '</span></div>' +
    (img ? '<img src="' + img + '">' : '') + '<div class="bar" style="animation-duration:' + TOAST_MS + 'ms"></div>';
  const close = () => { t.classList.add('out'); setTimeout(() => t.remove(), 350); };
  t.onclick = () => { close(); show('alerts'); };
  $('toasts').prepend(t);
  while ($('toasts').children.length > 3) $('toasts').lastChild.remove();
  setTimeout(close, TOAST_MS);
  try { navigator.vibrate && navigator.vibrate(e.kind === 'tamper' ? [500, 150, 500] : [250, 100, 250]); } catch (_) {}
  try { const a = new (window.AudioContext || window.webkitAudioContext)(), o = a.createOscillator(), g = a.createGain();
    o.type = 'sine'; o.frequency.value = e.kind === 'tamper' ? 560 : 880; o.connect(g); g.connect(a.destination);
    g.gain.setValueAtTime(.15, a.currentTime); g.gain.exponentialRampToValueAtTime(.001, a.currentTime + .5);
    o.start(); o.stop(a.currentTime + .5); setTimeout(() => a.close(), 700); } catch (_) {}
}

setInterval(status, 1000); setInterval(events, 2000); status(); events();
document.addEventListener('visibilitychange', () => { if (!document.hidden) { status(); events(); } });

// ---- notifications + install ----
let reg = null;
if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js', {scope: '/'}).then(r => { reg = r; notifState(); }).catch(() => notifState());
else notifState();
function b64(s) { const p = '='.repeat((4 - s.length % 4) % 4); const r = atob((s + p).replace(/-/g, '+').replace(/_/g, '/')); return Uint8Array.from([...r].map(c => c.charCodeAt(0))); }
async function notifState() {
  if (!('Notification' in window) || !('PushManager' in window)) { $('nstate').textContent = 'Not supported here - use Chrome on Android.'; return; }
  if (!window.isSecureContext) { $('nstate').textContent = 'Needs the https:// (Azure) address.'; return; }
  const sub = reg && await reg.pushManager.getSubscription();
  if (Notification.permission === 'denied') $('nstate').textContent = 'Blocked - allow notifications in Chrome site settings.';
  else $('nstate').textContent = sub ? 'On - alerts arrive even when the app is closed.' : 'Off - turn on to get alerts on this phone.';
  $('notif').textContent = sub ? 'Notifications are on' : 'Turn on notifications';
}
$('notif').onclick = async () => {
  try {
    if (!reg) reg = await navigator.serviceWorker.register('/sw.js', {scope: '/'});
    const k = await (await fetch('/push/key?' + q())).json();
    if (!k.enabled) { $('nstate').textContent = 'Server has notifications disabled.'; return; }
    if (await Notification.requestPermission() !== 'granted') { notifState(); return; }
    let sub = await reg.pushManager.getSubscription();
    if (!sub) sub = await reg.pushManager.subscribe({userVisibleOnly: true, applicationServerKey: b64(k.publicKey)});
    const r = await fetch('/push/subscribe?' + q(), {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(sub)});
    if (r.ok) notifState(); else $('nstate').textContent = 'Could not register (' + r.status + ').';
  } catch (e) { $('nstate').textContent = 'Error: ' + e.message; }
};
$('test').onclick = async () => {
  const r = await fetch('/push/test?' + q(), {method: 'POST'}); const d = await r.json();
  $('nstate').textContent = d.push ? 'Test sent to ' + d.phones + ' phone(s).' : 'Test added to the alert list.';
};
let deferred = null;
window.addEventListener('beforeinstallprompt', e => { e.preventDefault(); deferred = e; $('install').classList.remove('hide'); });
$('install').onclick = async () => { if (!deferred) return; deferred.prompt(); await deferred.userChoice; deferred = null; $('install').classList.add('hide'); };
</script></body></html>
"""
