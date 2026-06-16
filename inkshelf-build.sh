#!/usr/bin/env bash
#
# inkshelf-build.sh — обновить исходники, собрать под PocketBook (ARM) и залить
# на подключённую читалку.
#
# Использование:
#   ./inkshelf-build.sh           # собрать и залить
#   ./inkshelf-build.sh --pull    # сначала git pull, потом собрать и залить
#   ./inkshelf-build.sh --no-copy # собрать, но не копировать на читалку
#
# Путь к SDK можно переопределить переменной окружения PB_SDK_ROOT:
#   PB_SDK_ROOT=/path/to/SDK-B288 ./inkshelf-build.sh
#
set -euo pipefail

# ---- пути ------------------------------------------------------------------
# PROJECT определяется по расположению самого скрипта (он лежит в корне репо),
# поэтому работает из любого клона без правок.
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${PB_SDK_ROOT:-$HOME/pocketbook-sdk/SDK-B288}"
SYSROOT="$SDK/usr/arm-obreey-linux-gnueabi/sysroot"
CC="$SDK/usr/bin/arm-obreey-linux-gnueabi-gcc"
CXX="$SDK/usr/bin/arm-obreey-linux-gnueabi-g++"

DO_PULL=0
DO_COPY=1
for arg in "$@"; do
  case "$arg" in
    --pull)    DO_PULL=1 ;;
    --no-copy) DO_COPY=0 ;;
    *) echo "неизвестный аргумент: $arg" >&2; exit 2 ;;
  esac
done

if [ ! -x "$CC" ]; then
  echo "!! кросс-компилятор не найден: $CC" >&2
  echo "   Укажи путь к SDK через PB_SDK_ROOT=/path/to/SDK-B288 (ветка 6.5" >&2
  echo "   репозитория pocketbook/SDK_6.3.0). Подробности в README.md." >&2
  exit 1
fi

export PATH="$SDK/usr/bin:$PATH"
cd "$PROJECT"

# ---- 1. (опц.) обновить исходники ------------------------------------------
if [ "$DO_PULL" = 1 ]; then
  echo ">> git pull"
  git pull
  rm -rf build
fi

# ---- 2. конфигурация (только если build/ ещё нет) --------------------------
if [ ! -d build ]; then
  echo ">> cmake configure"
  cmake -B build \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
fi

# ---- 3. сборка -------------------------------------------------------------
echo ">> build"
cmake --build build

APP="$PROJECT/build/inkshelf.app"

# ---- 4. проверка, что бинарь ARM -------------------------------------------
echo ">> проверка типа бинаря:"
file "$APP"
file "$APP" | grep -q "ELF 32-bit.*ARM" || { echo "!! бинарь НЕ ARM"; exit 1; }

# ---- 5. (опц.) копирование на читалку --------------------------------------
if [ "$DO_COPY" = 1 ]; then
  MOUNT=""
  for d in /media/"$USER"/*/; do
    [ -d "${d}applications" ] && MOUNT="$d" && break
  done

  if [ -n "$MOUNT" ]; then
    cp "$APP" "${MOUNT}applications/"
    sync
    echo ">> залито: ${MOUNT}applications/inkshelf.app"
    echo ">> безопасно извлеки устройство перед отключением."
  else
    echo "!! читалка не примонтирована под /media/$USER."
    echo "   Подключи по USB и запусти снова."
  fi
fi

echo ">> готово."
