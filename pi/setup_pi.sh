#!/bin/bash
# One-time setup on the Raspberry Pi (Raspberry Pi OS Bookworm, 32-bit or 64-bit).
# Run from inside the pi/ folder:   bash setup_pi.sh
set -e
cd "$(dirname "$0")"
APP_DIR="$(pwd)"

echo "== Installing system packages =="
sudo apt update
sudo apt install -y python3-venv python3-opencv python3-picamera2 python3-gpiozero python3-requests

echo "== Creating Python environment =="
python3 -m venv --system-site-packages venv
./venv/bin/pip install --upgrade pip
./venv/bin/pip install "websockets>=12"

echo "== Installing auto-start service =="
sed -e "s|__APP_DIR__|$APP_DIR|g" -e "s|__USER__|$USER|g" mask-alert.service | \
    sudo tee /etc/systemd/system/mask-alert.service > /dev/null
sudo systemctl daemon-reload

echo
echo "Done."
echo "1) Edit config.env (SERVER_URL, API_KEY)"
echo "2) Test:       set -a; source config.env; set +a; ./venv/bin/python stream_client.py"
echo "3) Auto-start: sudo systemctl enable --now mask-alert"
echo "   Logs:       journalctl -u mask-alert -f"
