# inkshelf

![Downloads](https://img.shields.io/github/downloads/N-Combinator/inkshelf/total)

Native PocketBook OPDS browser + WiFi book drop — **no KOReader, no jailbreak**.

inkshelf is a single `.app` built against the official PocketBook InkView SDK.
It runs on stock firmware: copy `inkshelf.app` onto the SD card and launch it
from the device's application menu.

**Contents**

- [Features](#features)
- [Using the app](#using-the-app)
- [Troubleshooting](#troubleshooting)
- [Building & installing from source](#building--installing-from-source)
- [Testing](#testing)
- [Project layout](#project-layout)

## Features

- **OPDS catalog browser** — point it at any OPDS feed, browse, filter and
  download books straight into the native PocketBook library. Ships with
  presets for [Project Gutenberg](https://www.gutenberg.org) and
  [Flibusta](https://flibusta.is/opds) (Russian-language), plus a custom-URL
  entry via the on-screen keyboard. Every list has a tap-to-filter bar (by
  title/author); long catalogs page with the hardware page-turn keys.
- **WiFi book drop** — starts a tiny HTTP server on the reader; from a PC or
  phone on the same WiFi you open the shown URL and upload `epub`/`fb2` files
  that then appear in the native PocketBook library.
- **Minimal e-ink UI** drawn directly with the InkView API — a nav-stack of
  screens with a reusable list widget, mapped onto the PocketBook key matrix.

## Using the app

Launch **inkshelf** from the device's Applications menu. The home screen shows
an about block (name, version, repo) above two large buttons — pick one with the
hardware up/down keys and OK, or tap it.

### OPDS catalog

Pick a preset (Project Gutenberg, Flibusta) or **Custom URL...** and type a feed
address. Browse the catalog, tap the filter bar to narrow a list, or press
**Menu** to search. Open a book and confirm to download it straight into the
native PocketBook library.

### WiFi Book Drop

1. Make sure the reader is on WiFi, then open **WiFi Book Drop**. inkshelf
   starts an HTTP upload server (default port `8080`) and shows the URL, e.g.
   `http://192.168.1.42:8080`.
2. On a PC or phone on the same network, open that URL in a browser and upload
   one or more `epub`/`fb2` files.
3. Uploaded books are written to the device library and appear in the native
   PocketBook library. Leave the screen to stop the server.

### PIN protection

Every upload and over-the-air deploy requires a 4-digit PIN.

- **First visit** — if no PIN has been saved yet, the screen prompts you to set
  one via the numeric keyboard before the server starts. The PIN is shown on the
  screen next to the URL so you can type it into the browser.
- **Change PIN** — tap *Change PIN* (or press OK) while the server is running.
- **Storage** — saved in `/mnt/ext1/system/config/inkshelf.conf`, reloaded on
  each visit.
- **Protocol** — all `POST /drop` and `POST /deploy` requests must include the
  header `X-Inkshelf-PIN: <pin>`. The browser upload page sends it automatically;
  scripts pass `--pin <PIN>`. A missing or wrong PIN returns `403 Forbidden`.

## Troubleshooting

### WiFi drops while the app is open

PocketBook firmware powers the WiFi radio down on an idle timer to save battery,
so the connection can silently drop after you sit on a screen or read for a
while — not a bug in inkshelf itself.

inkshelf works around this automatically: it re-asserts the WiFi link before
each OPDS fetch / download (retrying once the radio is back) and before starting
the WiFi Book Drop server, so an idle drop usually recovers on its own with at
most a short pause.

If a request still fails with a connect/DNS/timeout error (shown as
`curl <n>: …`), the radio was likely mid-reconnect — just **retry the action**;
it normally succeeds on the second try. If it keeps failing:

- leave and re-enter the screen (or re-open inkshelf) to force a fresh connect;
- toggle WiFi off/on in the reader's status bar;
- in the reader's *Settings → Connectivity*, raise or disable the
  "disconnect when idle" timeout so the firmware stops powering the radio down.

## Building & installing from source

inkshelf cross-compiles with the `arm-obreey-linux-gnueabi` toolchain from the
official [PocketBook SDK_6.3.0](https://github.com/pocketbook/SDK_6.3.0). The
output is always a single `build/inkshelf.app` (an ARM 32-bit ELF).

> **TL;DR** — on a Linux x86_64 host with the SDK in place:
> `./inkshelf-build.sh` builds and copies over USB, or
> `./inkshelf-build-wifi.sh --find --pull --pin <PIN>` builds and deploys over WiFi.

### 1. Get the SDK (on the `6.5` branch, not `master`)

The repo's default `master` branch contains **only a README** — the actual SDK
lives on the `6.5` branch under `SDK-B288/`. (A plain `git clone` still pulls
~670 MB because it downloads every branch's objects.)

```bash
git clone https://github.com/pocketbook/SDK_6.3.0 ~/pocketbook-sdk
cd ~/pocketbook-sdk && git checkout 6.5      # SDK-B288/ now exists
```

The compiler is `SDK-B288/usr/bin/arm-obreey-linux-gnueabi-gcc` and the InkView
sysroot is `SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot`. **The toolchain
binaries are Linux x86_64 ELF** — they run on a Linux x86_64 host only (not
natively on macOS; use a `linux/amd64` container there).

### 2a. One-command build (recommended, Linux x86_64)

`inkshelf-build.sh` (repo root) runs the whole loop: cmake configure →
cross-compile → verify the output is an ARM ELF → copy `inkshelf.app` onto a
USB-connected reader.

```bash
chmod +x inkshelf-build.sh        # once, after cloning
./inkshelf-build.sh               # build + copy to the connected reader
./inkshelf-build.sh --pull        # git pull first, then a clean rebuild + copy
./inkshelf-build.sh --no-copy     # build only, don't touch the device
```

It finds the project from its own location, so it works from any clone. It
expects the SDK at `~/pocketbook-sdk/SDK-B288`; override with
`PB_SDK_ROOT=/path/to/SDK-B288 ./inkshelf-build.sh`. The reader is auto-detected
under `/media/$USER/*/` (must expose an `applications/` folder).

### 2b. Wireless build & deploy

`inkshelf-build-wifi.sh` is the USB-free counterpart to 2a: it (optionally) builds
and pushes the binary straight to the running app over WiFi via `POST /deploy`
(atomic and PIN-guarded).

> **Updates only — requires inkshelf already installed.** `/deploy` is inkshelf's
> own **WiFi Book Drop** feature, so this path only works to *update* a reader
> that already runs inkshelf. Do the **first** install over USB (2a or 2c); after
> that you can deploy over WiFi.

Open the **WiFi Book Drop** screen on the reader first so its server is listening,
then:

```bash
./inkshelf-build-wifi.sh --find --pin 1234          # deploy the current build
./inkshelf-build-wifi.sh --find --build --pin 1234  # build, then deploy
./inkshelf-build-wifi.sh --find --pull  --pin 1234  # git pull + clean rebuild, then deploy
```

`--find` locates the reader via mDNS or a local `/24` scan; `--build`/`--pull`
delegate to `inkshelf-build.sh` (single source of truth for the build). Use the
PIN shown on the reader. Jailbroken (PBJB) readers can instead push with
`make deploy` / `make deploy-nc` (see the `Makefile`).

### 2c. Manual CMake

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-obreey.cmake \
  -DPB_SDK_ROOT=/path/to/SDK-B288
cmake --build build --parallel
```

### A note on HTTPS / TLS

PocketBook firmware's libcurl is built against **NSS**, not OpenSSL. NSS ignores
`CURLOPT_CAINFO` pointed at a PEM bundle (it expects an NSS certificate
database), so supplying a CA file always failed the handshake with
`CURLE_SSL_CACERT_BADFILE` (curl error 77). inkshelf therefore disables TLS
peer/host verification (`CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` = 0) rather than
shipping an NSS trust DB. That is an accepted trade-off here: it only fetches
public OPDS feeds and public-domain books and never sends credentials or writes
data, so there is nothing for a man-in-the-middle to steal. No CA bundle needs to
be installed on the device.

### Installing over USB (no jailbreak)

To install without the build script (a prebuilt `.app`, or after a manual CMake
build), connect the reader over USB or pull its SD card, copy `build/inkshelf.app`
into the `applications/` folder, eject, and launch **inkshelf** from the
Applications menu. (2a does this automatically over USB; 2b does it over WiFi.)

## Testing

The parsing and server logic is covered by a host test gate that needs
**neither the PocketBook SDK nor a network connection** — it generates shim
`inkview.h` / libcurl headers so the repo stays self-contained:

```bash
make test            # or: tests/run_host_tests.sh
```

It runs, under AddressSanitizer + UBSan:

- unit tests for the dependency-free OPDS parsing code (`xml.c` + `opds.c`),
- unit tests for the upload HTTP server (`httpd.c`),
- an integration smoke test that drives the whole app (catalog → browse →
  search → book detail and back) against stub InkView + libcurl.

## Project layout

```
src/
  main.c            event-loop entry point
  app.{c,h}         screen nav-stack (push/pop/repaint, key/pointer dispatch)
  ui.{c,h}          fonts, header/footer chrome, paged list widget
  screens.{c,h}     main menu + screen wiring
  opds.{c,h}        OPDS Atom feed model + link/entry classification
  xml.{c,h}         dependency-free SAX-style XML parser
  http.{c,h}        libcurl HTTP fetch helpers
  opds_ui.c         OPDS browser UI (catalog picker, browse, search, detail)
  download.{c,h}    book download to the device library
  library.{c,h}     library paths + PocketBook library rescan
  httpd.{c,h}       WiFi-drop embedded HTTP upload server
  config.{c,h}      flat key=value config (PIN storage, inkshelf.conf)
cmake/                    arm-obreey cross-compile toolchain file
tests/                    host test gate (no SDK / no network)
build.sh                  Docker / direct build wrapper (CI-friendly)
inkshelf-build.sh         one-command build + USB deploy (dev)
inkshelf-build-wifi.sh    build (optional) + wireless deploy via /deploy (or scp)
Makefile                  build/test + jailbreak deploy (make deploy / deploy-nc)
```

## License

MIT.
