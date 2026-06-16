#!/usr/bin/env bash
#
# inkshelf-deploy-wifi.sh — залить inkshelf.app на PocketBook по WiFi.
#
# Использование:
#   ./inkshelf-deploy-wifi.sh <IP>           # деплой по HTTP (deploy endpoint)
#   ./inkshelf-deploy-wifi.sh <IP> --ssh     # деплой по SSH/SCP (если включён SSH)
#   ./inkshelf-deploy-wifi.sh --find         # найти IP читалки в локальной сети
#
# Пример:
#   ./inkshelf-deploy-wifi.sh 192.168.1.42
#
set -euo pipefail

PROJECT="$HOME/Documents/cprite/inkshelf"
APP="$PROJECT/build/inkshelf.app"
INKSHELF_PORT=8080          # порт HTTP-сервера inkshelf (WiFi drop)
DEPLOY_ENDPOINT="/deploy"   # endpoint для деплоя .app (добавлен в inkshelf)
SSH_USER="root"
SSH_PORT=22
APP_DEST="/mnt/ext1/applications/inkshelf.app"

# ---- вспомогательная функция: найти читалку в сети -------------------------
find_device() {
  echo ">> сканирую локальную сеть..."
  if command -v avahi-browse &>/dev/null; then
    avahi-browse -t _inkshelf._tcp 2>/dev/null | grep -oP '\d+\.\d+\.\d+\.\d+' | head -1
  else
    # arp-scan как fallback
    if command -v arp-scan &>/dev/null; then
      sudo arp-scan --localnet 2>/dev/null | grep -i "allwinner\|pocketbook" | awk '{print $1}' | head -1
    else
      echo "!! установи avahi-utils или arp-scan для автопоиска" >&2
      echo "   sudo apt install avahi-utils" >&2
      exit 1
    fi
  fi
}

# ---- разбор аргументов -----------------------------------------------------
if [ $# -eq 0 ]; then
  echo "Использование: $0 <IP> [--ssh] | --find"
  exit 1
fi

USE_SSH=0
PB_IP=""

for arg in "$@"; do
  case "$arg" in
    --find) PB_IP=$(find_device); echo ">> найдено: $PB_IP" ;;
    --ssh)  USE_SSH=1 ;;
    *)      PB_IP="$arg" ;;
  esac
done

if [ -z "$PB_IP" ]; then
  echo "!! не удалось определить IP читалки" >&2
  exit 1
fi

# ---- проверка наличия бинаря -----------------------------------------------
if [ ! -f "$APP" ]; then
  echo "!! $APP не найден — сначала запусти inkshelf-build.sh" >&2
  exit 1
fi
file "$APP" | grep -q "ELF 32-bit.*ARM" || { echo "!! бинарь не ARM — пересобери" >&2; exit 1; }
echo ">> деплоим $(basename "$APP") ($(du -sh "$APP" | cut -f1)) на $PB_IP"

# ---- деплой ----------------------------------------------------------------
if [ "$USE_SSH" = 1 ]; then
  # SSH/SCP режим (работает если на PocketBook включён sshd)
  echo ">> режим: SCP → $SSH_USER@$PB_IP:$APP_DEST"
  scp -P "$SSH_PORT" "$APP" "$SSH_USER@$PB_IP:$APP_DEST"
  echo ">> перезапуск приложения через SSH..."
  ssh -p "$SSH_PORT" "$SSH_USER@$PB_IP" "killall inkshelf.app 2>/dev/null; sleep 1; $APP_DEST &" || true
else
  # HTTP режим через deploy endpoint inkshelf
  echo ">> режим: HTTP POST → http://$PB_IP:$INKSHELF_PORT$DEPLOY_ENDPOINT"
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
    -X POST \
    -F "file=@$APP;type=application/octet-stream" \
    "http://$PB_IP:$INKSHELF_PORT$DEPLOY_ENDPOINT" \
    --connect-timeout 5 \
    --max-time 60)

  if [ "$HTTP_STATUS" = "200" ]; then
    echo ">> успешно задеплоено — читалка перезапустит приложение автоматически"
  else
    echo "!! HTTP $HTTP_STATUS — убедись что inkshelf запущен на читалке и WiFi активен"
    echo "   Альтернатива: ./$(basename "$0") $PB_IP --ssh"
    exit 1
  fi
fi

echo ">> готово."
