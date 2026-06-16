#!/usr/bin/env bash
#
# inkshelf-deploy-wifi.sh — deploy inkshelf.app to PocketBook over WiFi.
#
# Usage:
#   ./inkshelf-deploy-wifi.sh <IP>             # deploy via HTTP /deploy endpoint
#   ./inkshelf-deploy-wifi.sh <IP> --pin 1234  # ...with the PIN shown on the reader
#   ./inkshelf-deploy-wifi.sh <IP> --ssh       # deploy via SSH/SCP (needs sshd/PBJB)
#   ./inkshelf-deploy-wifi.sh --find           # auto-detect device IP, then deploy
#
# Example:
#   ./inkshelf-deploy-wifi.sh 192.168.1.42 --pin 1234
#
# The project directory defaults to the directory this script lives in (the repo
# root), so it works from any clone location with no setup. Override with
# INKSHELF_DIR if the built binary lives elsewhere:
#   INKSHELF_DIR=~/projects/inkshelf ./inkshelf-deploy-wifi.sh 192.168.1.42
#
set -euo pipefail

# Default the project to *this script's own directory* (the repo root). This is
# what makes the script "just work" regardless of where the repo was cloned —
# previously it hard-coded ~/inkshelf and failed on any other layout.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="${INKSHELF_DIR:-$SCRIPT_DIR}"
APP="$PROJECT/build/inkshelf.app"
INKSHELF_PORT=8080
DEPLOY_ENDPOINT="/deploy"
SSH_USER="root"
SSH_PORT=22
APP_DEST="/mnt/ext1/applications/inkshelf.app"

# ---- auto-detect device on local network ------------------------------------
# Prints the device IP on stdout; all progress goes to stderr so it does not
# pollute the captured value.
find_device() {
  echo ">> scanning local network for inkshelf..." >&2

  # 1) mDNS — the reader advertises _inkshelf._tcp when the server is up.
  if command -v avahi-browse >/dev/null 2>&1; then
    local ip
    ip=$(avahi-browse -t -r -p _inkshelf._tcp 2>/dev/null \
         | awk -F';' '/^=/{print $8; exit}')
    if [ -n "${ip:-}" ]; then echo "$ip"; return 0; fi
  fi

  # 2) Fallback that needs no extra tooling: probe :PORT across the local /24
  #    and keep the host whose root page is the WiFi Book Drop form. This is the
  #    definitive signature of a running inkshelf server.
  local local_ip base tmp ip
  local_ip=$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K\S+' || true)
  if [ -n "${local_ip:-}" ]; then
    base=${local_ip%.*}
    echo ">> probing ${base}.1-254:${INKSHELF_PORT} for the WiFi Book Drop page..." >&2
    tmp=$(mktemp)
    for i in $(seq 1 254); do
      ( curl -s -m 1 "http://${base}.${i}:${INKSHELF_PORT}/" 2>/dev/null \
          | grep -q 'WiFi Book Drop' && echo "${base}.${i}" >>"$tmp" ) &
    done
    wait
    ip=$(head -n1 "$tmp" 2>/dev/null || true); rm -f "$tmp"
    if [ -n "${ip:-}" ]; then echo "$ip"; return 0; fi
  fi

  echo "!! could not find inkshelf on the network." >&2
  echo "   Make sure the reader is awake, on the same WiFi, and the" >&2
  echo "   WiFi Book Drop screen is open (the server only listens then)." >&2
  return 1
}

# ---- parse arguments --------------------------------------------------------
if [ $# -eq 0 ]; then
  echo "Usage: $0 <IP> [--pin N] [--ssh] | --find [--pin N] [--ssh]"
  exit 1
fi

USE_SSH=0
DO_FIND=0
PB_IP=""
PIN=""

# A real while-loop so "--pin <value>" (space form) consumes its argument
# correctly. The previous for-loop used `shift`, which does not advance a
# `for arg in "$@"` iteration, so the PIN leaked into PB_IP.
while [ $# -gt 0 ]; do
  case "$1" in
    --find)   DO_FIND=1 ;;
    --ssh)    USE_SSH=1 ;;
    --pin)    shift; PIN="${1:-}" ;;
    --pin=*)  PIN="${1#--pin=}" ;;
    -*)       echo "!! unknown option: $1" >&2; exit 2 ;;
    *)        PB_IP="$1" ;;
  esac
  shift
done

# Resolve --find after parsing so flag order does not matter.
if [ "$DO_FIND" = 1 ] && [ -z "$PB_IP" ]; then
  PB_IP=$(find_device)
  echo ">> found: $PB_IP"
fi

if [ -z "$PB_IP" ]; then
  echo "!! no device IP — pass one explicitly or use --find" >&2
  exit 1
fi

# ---- verify binary exists and is ARM ----------------------------------------
if [ ! -f "$APP" ]; then
  echo "!! $APP not found — run ./inkshelf-build.sh (or set INKSHELF_DIR) first" >&2
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
  PIN_ARGS=()
  [ -n "$PIN" ] && PIN_ARGS=(-H "X-Inkshelf-PIN:$PIN")

  # Capture body + status so a wrong PIN / closed screen gives a useful message.
  RESPONSE=$(curl -s -w $'\n%{http_code}' \
    -X POST \
    "${PIN_ARGS[@]}" \
    -F "file=@$APP;type=application/octet-stream" \
    "http://$PB_IP:$INKSHELF_PORT$DEPLOY_ENDPOINT" \
    --connect-timeout 5 \
    --max-time 120) || true
  HTTP_STATUS=${RESPONSE##*$'\n'}
  BODY=${RESPONSE%$'\n'*}

  case "$HTTP_STATUS" in
    200)
      echo ">> deployed successfully — device will restart the app automatically"
      [ -n "$BODY" ] && echo "   device: $BODY"
      ;;
    403)
      echo "!! HTTP 403 — wrong or missing PIN. Pass --pin <the number on the reader>." >&2
      exit 1
      ;;
    000)
      echo "!! no connection to $PB_IP:$INKSHELF_PORT — the reader is offline, asleep," >&2
      echo "   or the WiFi Book Drop screen is not open. Wake it and retry (or --find)." >&2
      exit 1
      ;;
    *)
      echo "!! HTTP $HTTP_STATUS — ${BODY:-deploy failed}" >&2
      echo "   Make sure inkshelf is running with WiFi Book Drop open. Alt: $0 $PB_IP --ssh" >&2
      exit 1
      ;;
  esac
fi

echo ">> done."
