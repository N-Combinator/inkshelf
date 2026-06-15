#!/usr/bin/env bash
#
# Host test gate. Runs without the PocketBook SDK or a network connection:
#
#   1. Unit tests for the dependency-free parsing code (xml.c + opds.c).
#   2. An integration smoke test that runs the whole app (all screens) against
#      stub InkView + libcurl, driving catalog -> browse -> search ->
#      book-detail and back, under AddressSanitizer + UBSan.
#
# Shim headers for inkview.h / curl are generated here so the repo stays
# self-contained for host testing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-cc}"
SAN="-fsanitize=address,undefined -g"
WARN="-std=c11 -Wall -Wextra -Werror"
OUT="$(mktemp -d)"
trap 'rm -rf "${OUT}"' EXIT

INC="${OUT}/include"
mkdir -p "${INC}/curl"

cat > "${INC}/inkview.h" <<'EOF'
#ifndef INKVIEW_H
#define INKVIEW_H
#include <stddef.h>
typedef struct ifont ifont;
typedef void (*iv_kbdhandler)(char *);
#define BLACK 0x00
#define DGRAY 0x55
#define LGRAY 0xAA
#define WHITE 0xFF
#define ALIGN_LEFT 0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT 2
#define VALIGN_TOP 0
#define VALIGN_MIDDLE 8
#define VALIGN_BOTTOM 16
#define ICON_INFORMATION 1
#define ICON_QUESTION 2
#define ICON_WARNING 3
#define ICON_ERROR 4
enum { EVT_INIT, EVT_SHOW, EVT_KEYPRESS, EVT_EXIT, EVT_POINTERUP, EVT_POINTERDOWN };
enum { KEY_BACK, KEY_HOME, KEY_OK, KEY_PREV, KEY_NEXT, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_MENU };
int ScreenWidth(void);
int ScreenHeight(void);
void ClearScreen(void);
ifont *OpenFont(const char *name, int size, int aa);
void CloseFont(ifont *f);
void SetFont(ifont *f, int color);
void DrawTextRect(int x,int y,int w,int h,const char *s,int flags);
void DrawString(int x,int y,const char *s);
void DrawLine(int x1,int y1,int x2,int y2,int color);
void DrawRect(int x,int y,int w,int h,int color);
void FillArea(int x,int y,int w,int h,int color);
void FullUpdate(void);
void PartialUpdate(int x,int y,int w,int h);
void CloseApp(void);
void OpenKeyboard(const char *title, char *buf, int maxlen, int flags, iv_kbdhandler h);
int Message(int icon, const char *title, const char *text, int timeout);
void SendEvent(void *hproc, int type, int par1, int par2);
void InkViewMain(int (*h)(int,int,int));
#endif
EOF

cat > "${INC}/curl/curl.h" <<'EOF'
#ifndef CURL_H
#define CURL_H
#include <stddef.h>
typedef void CURL;
typedef enum { CURLE_OK = 0 } CURLcode;
#define CURL_ERROR_SIZE 256
#define CURLOPT_URL 1
#define CURLOPT_WRITEFUNCTION 2
#define CURLOPT_WRITEDATA 3
#define CURLOPT_FOLLOWLOCATION 4
#define CURLOPT_MAXREDIRS 5
#define CURLOPT_USERAGENT 6
#define CURLOPT_TIMEOUT 7
#define CURLOPT_CONNECTTIMEOUT 8
#define CURLOPT_NOSIGNAL 9
#define CURLOPT_ERRORBUFFER 10
#define CURLOPT_ACCEPT_ENCODING 11
#define CURLOPT_PROGRESSFUNCTION 12
#define CURLOPT_PROGRESSDATA 13
#define CURLOPT_NOPROGRESS 14
#define CURLINFO_RESPONSE_CODE 100
typedef int (*curl_progress_callback)(void *, double, double, double, double);
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *, int, ...);
CURLcode curl_easy_perform(CURL *);
CURLcode curl_easy_getinfo(CURL *, int, ...);
void curl_easy_cleanup(CURL *);
const char *curl_easy_strerror(CURLcode);
#endif
EOF

echo "== unit tests (xml + opds) =="
# shellcheck disable=SC2086
"${CC}" ${WARN} ${SAN} -I"${ROOT}/src" \
    "${ROOT}/src/xml.c" "${ROOT}/src/opds.c" "${ROOT}/tests/test_opds.c" \
    -o "${OUT}/test_opds"
"${OUT}/test_opds"

echo
echo "== unit tests (httpd: multipart parser + filename safety) =="
# httpd.c is socket/pthread code but the parser core runs over a memory reader,
# so the unit test never opens a socket. -pthread satisfies the link.
# shellcheck disable=SC2086
"${CC}" ${WARN} ${SAN} -pthread -I"${ROOT}/src" \
    "${ROOT}/src/httpd.c" "${ROOT}/src/library.c" "${ROOT}/tests/test_httpd.c" \
    -o "${OUT}/test_httpd"
"${OUT}/test_httpd"

echo
echo "== integration smoke (full app, stub InkView+curl) =="
# All app sources except main.c's real InkView/curl come from the stubs.
# download.c is excluded — host_stubs.c provides a stub download_book,
# and http.c (which needs real curl) is replaced by the curl stubs in host_stubs.c.
# shellcheck disable=SC2086
"${CC}" ${WARN} ${SAN} -I"${ROOT}/src" -I"${INC}" \
    "${ROOT}/src/app.c" \
    "${ROOT}/src/http.c" \
    "${ROOT}/src/library.c" \
    "${ROOT}/src/main.c" \
    "${ROOT}/src/opds.c" \
    "${ROOT}/src/opds_ui.c" \
    "${ROOT}/src/screens.c" \
    "${ROOT}/src/ui.c" \
    "${ROOT}/src/xml.c" \
    "${ROOT}/tests/host_stubs.c" \
    -o "${OUT}/app_smoke"
"${OUT}/app_smoke"
echo "integration smoke: OK (no leaks / UB reported)"
