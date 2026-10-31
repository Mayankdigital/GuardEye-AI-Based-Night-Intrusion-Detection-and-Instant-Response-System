#!/bin/bash
# One-time setup of the LIGHT (ONNX) server on a 1 GB Azure Ubuntu VM.
# Usage (on the VM, inside the azure_server folder):
#     bash setup_azure_vm_1gb.sh YOUR-LABEL.centralindia.cloudapp.azure.com [API_KEY]
# (no API_KEY given -> a random one is created and printed at the end)
set -e
cd "$(dirname "$0")"
APP_DIR="$(pwd)"
DOMAIN="$1"
API_KEY="${2:-$(python3 -c "import secrets;print('mk-'+secrets.token_hex(8))")}"

if [ -z "$DOMAIN" ]; then
    echo "Usage: bash setup_azure_vm_1gb.sh YOUR-LABEL.centralindia.cloudapp.azure.com"
    exit 1
fi
[ -f best.onnx ] || { echo "ERROR: best.onnx not found in $APP_DIR (run export_onnx.bat on the laptop first)"; exit 1; }

echo "== 1/6 Swap file (safety for 1 GB RAM) =="
if [ ! -f /swapfile ]; then
    sudo fallocate -l 2G /swapfile
    sudo chmod 600 /swapfile
    sudo mkswap /swapfile
    sudo swapon /swapfile
    echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab >/dev/null
fi

echo "== 2/6 System packages =="
sudo apt-get update
sudo apt-get install -y python3-venv python3-pip curl gnupg

echo "== 3/6 Caddy (free HTTPS / wss) =="
if ! apt-cache show caddy >/dev/null 2>&1; then
    sudo apt-get install -y debian-keyring debian-archive-keyring apt-transport-https
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | \
        sudo gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | \
        sudo tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
    sudo apt-get update
fi
sudo apt-get install -y caddy

echo "== 4/6 Python packages (2-3 min) =="
python3 -m venv venv
./venv/bin/pip install --upgrade pip
./venv/bin/pip install -r requirements.txt

echo "== 5/6 Server service (starts on every boot) =="
sudo tee /etc/systemd/system/mask-server.service >/dev/null <<EOF
[Unit]
Description=Covered face ONNX server
Wants=network-online.target
After=network-online.target

[Service]
User=$USER
WorkingDirectory=$APP_DIR
Environment=API_KEY=$API_KEY
Environment=MODEL_PATH=best.onnx
Environment=CONF=0.5
Environment=THREADS=2
ExecStart=$APP_DIR/venv/bin/uvicorn app_onnx:app --host 127.0.0.1 --port 7860
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable mask-server
sudo systemctl restart mask-server

echo "== 6/6 HTTPS for $DOMAIN =="
sudo tee /etc/caddy/Caddyfile >/dev/null <<EOF
$DOMAIN {
    reverse_proxy 127.0.0.1:7860
}
EOF
sudo systemctl enable caddy
sudo systemctl restart caddy

echo "Waiting for the server..."
OK=0
for i in $(seq 1 20); do
    if curl -s http://127.0.0.1:7860/health | grep -q '"ok":true'; then OK=1; break; fi
    sleep 3
done

echo
if [ "$OK" = "1" ]; then
    echo "================================================================"
    echo " SERVER IS RUNNING  (ONNX, light version)"
    curl -s http://127.0.0.1:7860/health; echo
    echo " Health:  https://$DOMAIN/health"
    echo " Viewer:  https://$DOMAIN/?key=$API_KEY"
    echo
    echo " On the Pi, in ~/pi/config_azure.env:"
    echo "     SERVER_URL=wss://$DOMAIN"
    echo "     API_KEY=$API_KEY"
    echo "================================================================"
    echo "(HTTPS certificate can take 1-2 minutes the first time)"
    free -h
else
    echo "Server did not start. See the error with:"
    echo "    journalctl -u mask-server -n 50 --no-pager"
fi
