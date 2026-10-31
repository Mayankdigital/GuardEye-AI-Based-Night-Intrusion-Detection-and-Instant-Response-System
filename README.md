# SecureHome

Raspberry Pi camera security system:

- **Covered-face detection** - a YOLO11 model detects faces covered by a mask, scarf, hand etc. (`face` / `covered_face`)
- **Camera tamper detection** - IR proximity sensor + OpenCV sharpness/contrast check: if something comes close to the
  camera and the view then stays blocked or blurred for 15 s, an alert is raised
- **SecureHome phone app** (installable web app) - live video, alert history with snapshots, push notifications
- Runs with the detection server on a **laptop (same Wi-Fi)** or on an **Azure VM** (works from anywhere)

```
Raspberry Pi 3 (camera, IR sensor, buzzer) --frames + alerts over WebSocket--> server (YOLO / ONNX)
                                                                                  |
                                                       SecureHome app <-----------+  (live video, alerts, push)
```

## Folders

| Folder | What it is |
|---|---|
| `training/` | Google Colab notebook: trains the model on public Roboflow datasets |
| `model/` | Trained model: `best.pt` (laptop server) and `best.onnx` (Azure light server) |
| `laptop_server/` | FastAPI + PyTorch server for Windows (`start_server.bat`) |
| `azure_server/` | Light FastAPI + ONNX Runtime server for a 1 GB Azure VM, HTTPS via Caddy |
| `pi/` | Raspberry Pi client: camera streaming, buzzer, tamper detection |
| `docs/` | Step-by-step setup guides |

## Keys and secrets

No real keys are in this repository. Before running:

- **Laptop server:** edit `laptop_server/start_server.bat` and set `API_KEY` to a long random value.
- **Azure server:** `bash setup_azure_vm_1gb.sh YOUR-DNS-NAME.centralindia.cloudapp.azure.com [API_KEY]`
  (leave out the key and a random one is created and printed).
- **Pi:** copy `pi/config.example.env` to `pi/config.env` and `pi/config_azure.example.env` to `pi/config_azure.env`,
  then fill in your server address and the same key. These files are in `.gitignore`.

Files the servers create at runtime (`vapid_private.pem`, `push_subscriptions.json`) are secrets too and are ignored.

## Hardware (Raspberry Pi 3 Model B)

| Part | Pi pin |
|---|---|
| Pi Camera (OV5647) | CSI camera connector |
| Buzzer + / - | pin 11 (GPIO17) / pin 6 (GND) |
| IR tracking sensor S / V+ / G | pin 15 (GPIO22) / pin 17 (3.3V) / pin 14 (GND) |

## Quick start

See `docs/` for full steps:
`LAPTOP_SETUP.txt`, `AZURE_1GB_SETUP.txt`, `PHONE_APP_SETUP.txt`.
