#!/bin/bash
# Update the Azure server to the version with the phone app + notifications.
# Run on the VM inside ~/azure_server after copying the new files:
#     bash update_server.sh
set -e
cd "$(dirname "$0")"
echo "== Installing new packages =="
./venv/bin/pip install -r requirements.txt
echo "== Restarting server =="
sudo systemctl restart mask-server
for i in $(seq 1 20); do
    if curl -s http://127.0.0.1:7860/health | grep -q '"ok":true'; then OK=1; break; fi
    sleep 2
done
if [ "$OK" = "1" ]; then
    echo "Server running."
    journalctl -u mask-server -n 20 --no-pager | grep -iE "push|phone app|Model loaded" || true
    echo
    echo "Phone app:  https://<your-dns-name>/app?key=<your API key>"
else
    echo "Server did not start:  journalctl -u mask-server -n 50 --no-pager"
fi
