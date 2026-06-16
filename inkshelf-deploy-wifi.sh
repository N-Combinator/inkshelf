#!/usr/bin/env bash
#
# inkshelf-deploy-wifi.sh — deploy inkshelf.app to PocketBook over WiFi.
#
# Usage:
#   ./inkshelf-deploy-wifi.sh <IP>                # deploy via HTTP (inkshelf deploy endpoint)
#   ./inkshelf-deploy-wifi.sh <IP> --pin 1234     # HTTP deploy, sending the WiFi-drop PIN
#   ./inkshelf-deploy-wifi.sh <IP> --ssh          # deploy via SSH/SCP (if sshd is enabled)
#   ./inkshelf-deploy-wifi.sh --find              # auto-detect device IP on local network
#
# Example:
#   ./inkshelf-deploy-wifi.sh 192.168.1.42 --pin 4271
#
# Set INKSHELF_DIR to override the default project path:
#   INKSHELF_DIR=~/projects/inkshelf ./inkshelf-deploy-wifi.sh 192.168.1.42
#
# The PIN may also be supplied via the INKSHELF_PIN environment variable
# (the --pin flag takes precedence). It is required whenever the device has a
# WiFi-drop PIN set — the server answers 403 without the correct one.
#
set -euo pipefail

PROJECT="${INKSHELF_DIR:-$HOME/inkshelf}"
APP="$PROJECT/build/inkshelf.app"
INKSHELF_PORT=8080
DEPLOY_ENDPOINT="/deploy"
SSH_USER="root"
SSH_PORT=22
APP_DEST="/mnt/ext1/applications/inkshelf.app"
PIN="${INKSHELF_PIN:-}"

# ---- auto-detect device on local network ------------------------------------
find_device() {
  echo ">> scanning local network..."
  if command -v avahi-browse &>/dev/null; then
    avahi-browse -t _inkshelf._tcp 2>/dev/null | grep -oP '\d+\.\d+\.\d+\.\d+' | head -1
  elif command -v arp-scan &>/dev/null; then
    sudo arp-scan --localnet 2>/dev/null | grep -i "allwinner\|pocketbook" | awk '{print $1}' | head -1
  else
    echo "!! install avahi-utils or arp-scan for auto-detect" >&2
    echo "   sudo apt install avahi-utils" >&2
    exit 1
  fi
}

# ---- parse arguments --------------------------------------------------------
if [ $# -eq 0 ]; then
  echo "Usage: $0 <IP> [--pin <PIN>] [--ssh] | --find"
  exit 1
fi

USE_SSH=0
PB_IP=""

while [ $# -gt 0 ]; do
  case "$1" in
    --find)    PB_IP=$(find_device); echo ">> found: $PB_IP" ;;
    --ssh)     USE_SSH=1 ;;
    --pin)     shift; PIN="${1:-}" ;;
    --pin=*)   PIN="${1#--pin=}" ;;
    -*)        echo "!! unknown argument: $1" >&2; exit 2 ;;
    *)         PB_IP="$1" ;;
  esac
  shift
done

if [ -z "$PB_IP" ]; then
  echo "!! could not determine device IP" >&2
  exit 1
fi

# ---- verify binary exists and is ARM ----------------------------------------
if [ ! -f "$APP" ]; then
  echo "!! $APP not found — run inkshelf-build.sh first" >&2
  exit 1
fi
file "$APP" | grep -q "ELF 32-bit.*ARM" || { echo "!! not an ARM binary — rebuild first" >&2; exit 1; }
echo ">> deploying $(basename "$APP") ($(du -sh "$APP" | cut -f1)) to $PB_IP"

# ---- deploy -----------------------------------------------------------------
if [ "$USE_SSH" = 1 ]; then
  echo ">> mode: SCP → $SSH_USER@$PB_IP:$APP_DEST"
  scp -P "$SSH_PORT" "$APP" "$SSH_USER@$PB_IP:$APP_DEST"
  echo ">> restarting app via SSH..."
  ssh -p "$SSH_PORT" "$SSH_USER@$PB_IP" \
    "killall inkshelf.app 2>/dev/null; sleep 1; $APP_DEST &" || true
else
  echo ">> mode: HTTP POST → http://$PB_IP:$INKSHELF_PORT$DEPLOY_ENDPOINT"
  # The deploy endpoint is PIN-gated: attach X-Inkshelf-PIN when we have one.
  PIN_HEADER=()
  if [ -n "$PIN" ]; then
    PIN_HEADER=(-H "X-Inkshelf-PIN: $PIN")
    echo ">> sending WiFi-drop PIN"
  fi
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
    -X POST \
    "${PIN_HEADER[@]}" \
    -F "file=@$APP;type=application/octet-stream" \
    "http://$PB_IP:$INKSHELF_PORT$DEPLOY_ENDPOINT" \
    --connect-timeout 5 \
    --max-time 60)

  if [ "$HTTP_STATUS" = "200" ]; then
    echo ">> deployed successfully — device will restart the app automatically"
  elif [ "$HTTP_STATUS" = "403" ]; then
    echo "!! HTTP 403 — wrong or missing PIN."
    echo "   Pass the PIN shown on the reader: $0 $PB_IP --pin <PIN>"
    exit 1
  else
    echo "!! HTTP $HTTP_STATUS — make sure inkshelf is running on the device and WiFi is active"
    echo "   Alternative: $0 $PB_IP --ssh"
    exit 1
  fi
fi

echo ">> done."
