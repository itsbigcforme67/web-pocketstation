#!/usr/bin/env bash
# Installs PocketSync as a background service for your user (starts at login).
# Usage: ./install.sh [path/to/pocketstation_bios.bin] [--token SECRET]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/pocketsync"
APP="$DATA/app"
ARGS=""
BIOS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --token) ARGS="$ARGS --token $2"; shift 2 ;;
    *) BIOS="$1"; shift ;;
  esac
done
command -v python3 >/dev/null || { echo "python3 is required (sudo apt install python3)"; exit 1; }
mkdir -p "$APP" "$DATA/bios" "$DATA/cards"
rm -rf "$APP/web" "$APP/companion"
cp -r "$HERE/web" "$HERE/companion" "$APP/"
if [ -n "$BIOS" ]; then
  if [ "$(stat -c %s "$BIOS")" = "16384" ]; then cp "$BIOS" "$DATA/bios/"; echo "BIOS copied to $DATA/bios/";
  else echo "warning: $BIOS is not 16 KB, skipped"; fi
fi
mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/pocketsync.service" <<UNIT
[Unit]
Description=PocketSync - DuckStation memory cards to Web PocketStation
After=network-online.target

[Service]
ExecStart=/usr/bin/env python3 $APP/companion/pocketsync.py --web-dir $APP/web$ARGS
Restart=on-failure

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now pocketsync.service
sleep 1
echo
echo "PocketSync is running. Extra cards/games folder: $DATA/cards"
echo "Addresses for your phone:"
journalctl --user -u pocketsync.service -n 30 --no-pager -o cat | grep -E "http://" || true
echo
echo "If your phone can't connect and you use a firewall:  sudo ufw allow 8765/tcp"
echo "Logs: journalctl --user -u pocketsync -f     Stop: systemctl --user disable --now pocketsync"
