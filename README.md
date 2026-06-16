# inkshelf

Native PocketBook OPDS browser + WiFi book drop — **no KOReader, no jailbreak**.

inkshelf is a single `.app` built against the official PocketBook InkView SDK.
It runs on stock firmware: copy `inkshelf.app` onto the SD card and launch it
from the device's application menu.

## What it does

- **OPDS catalog browser** — point it at any OPDS feed, browse, search and
  download books straight into the native PocketBook library. Ships with
  presets for [Standard Ebooks](https://standardebooks.org),
  [Project Gutenberg](https://www.gutenberg.org) and
  [Flibusta](https://flibusta.is/opds) (Russian-language), plus a custom-URL
  entry via the on-screen keyboard.
- **WiFi book drop** — starts a tiny HTTP server on the reader; from a PC or
  phone on the same WiFi you open the shown URL and upload `epub`/`fb2` files
  that then appear in the native PocketBook library.
- **Minimal e-ink UI** drawn directly with the InkView API — a nav-stack of
  screens with a reusable scrollable list widget, mapped onto the PocketBook
  key matrix.

## Using it

Launch **inkshelf** from the device's Applications menu. The main menu has two
entries:

### OPDS Catalog
1. Pick a preset (Standard Ebooks, Project Gutenberg, Flibusta) or
   **Custom URL...** and type an OPDS feed address with the on-screen keyboard.
2. Browse the feed; nested catalogs open inline, book entries open a detail
   view. Use the feed's search (where the catalog advertises it) to filter.
3. Open a book and confirm download — the file lands in the device library and
   the PocketBook library is rescanned so it shows up immediately.

### WiFi Book Drop
1. Make sure the reader is on WiFi, then open **WiFi Book Drop**. inkshelf
   starts an HTTP upload server (default port `8080`) and shows the URL, e.g.
   `http://192.168.1.42:8080`.
2. On a PC or phone on the same network, open that URL in a browser and upload
   one or more `epub`/`fb2` files.
3. Uploaded books are written to the device library and appear in the native
   PocketBook library. Leave the screen to stop the server.

## Building

inkshelf cross-compiles with the `arm-obreey-linux-gnueabi` toolchain from the
official [PocketBook SDK_6.3.0](https://github.com/pocketbook/SDK_6.3.0).

### Quick build

`inkshelf-build.sh` (repo root) does the whole loop in one shot: refresh the CA
bundle → cmake configure → cross-compile for ARM → verify the output is an ARM
ELF → copy `inkshelf.app` **and** `cacert.pem` onto a USB-connected reader.

```bash
chmod +x inkshelf-build.sh        # once, after cloning
./inkshelf-build.sh               # build + deploy to the connected reader
./inkshelf-build.sh --pull        # git pull first, then a clean rebuild + deploy
./inkshelf-build.sh --no-copy     # build only, don't touch the device
```

It finds the project from its own location, so it works from any clone. It
expects the SDK at `~/pocketbook-sdk/SDK-B288`; override with
`PB_SDK_ROOT=/path/to/SDK-B288 ./inkshelf-build.sh` (see *Get the SDK* below).
The device is auto-detected under `/media/$USER/*/` (must expose an
`applications/` folder). For CI or container builds, use `build.sh` instead.

### Get the SDK (it's on the `6.5` branch, not `master`)

The repo's default `master` branch contains **only a README** — the actual SDK
lives on the `6.5` branch under `SDK-B288/`. (A plain `git clone` still pulls
~670 MB because it downloads the objects for every branch.)

```bash
git clone https://github.com/pocketbook/SDK_6.3.0
cd SDK_6.3.0 && git checkout 6.5      # SDK-B288/ now exists
```

The compiler is `SDK-B288/usr/bin/arm-obreey-linux-gnueabi-gcc` and the InkView
sysroot is `SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot`.

### Build

**The toolchain binaries are Linux x86_64 ELF.** They run on a Linux x86_64
host only — *not natively on macOS*. On Linux x86_64:

```bash
PB_SDK_ROOT=/path/to/SDK-B288 ./build.sh
```

On **macOS** (or any non-x86_64-Linux host), build inside a `linux/amd64`
container with the SDK mounted — no custom image needed:

```bash
docker run --rm --platform linux/amd64 \
  -v "$PWD":/work -v /path/to/SDK-B288:/sdk -w /work ubuntu:22.04 \
  bash -c 'apt-get update -qq && apt-get install -y -qq cmake make >/dev/null \
           && PB_SDK_ROOT=/sdk ./build.sh'
```

(If you have prebuilt your own SDK image, `USE_DOCKER=1 PB_SDK_IMAGE=<img>
./build.sh` runs the build inside it instead.)

`build.sh` preflights and prints exactly what's missing (toolchain not found,
Docker not running, image absent) instead of failing obscurely. It then stages
an install-ready folder at `build/dist/` containing **both** `inkshelf.app` and
`cacert.pem` (the CA bundle, see below).

### HTTPS needs a CA bundle

PocketBook firmware ships no usable CA certificate bundle, so libcurl rejects
every HTTPS OPDS catalog with `CURLE_SSL_CACERT_BADFILE` (curl error 77) unless
a real bundle is present. inkshelf bundles the current Mozilla CA set
(`assets/cacert.pem`, from <https://curl.se/ca/cacert.pem>) and, at runtime,
looks for `cacert.pem` next to its own binary first, then at
`/mnt/ext1/system/config/cacert.pem`. Refresh it periodically by re-downloading
that file into `assets/`.

### Manual CMake invocation

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-obreey.cmake \
  -DPB_SDK_ROOT=/path/to/sdk
cmake --build build --parallel
```

## Installing (no jailbreak)

1. Connect the PocketBook to your computer over USB, or pull its SD card.
2. Copy `inkshelf.app` (from `build/dist/`) into the `applications/` folder on
   the device.
3. Install the CA bundle so HTTPS catalogs work. The confirmed device path is
   `/mnt/ext1/system/config/cacert.pem` (which shows up as
   `<mountpoint>/system/config/` over USB):

   ```bash
   curl -o cacert.pem https://curl.se/ca/cacert.pem
   # replace PB743G with your device's mount label
   cp cacert.pem /media/$USER/PB743G/system/config/cacert.pem
   ```

   (`assets/cacert.pem` in this repo is the same file if you'd rather copy that.
   Without it, HTTPS OPDS catalogs fail with curl error 77; WiFi book drop still
   works. inkshelf also accepts a `cacert.pem` sitting next to `inkshelf.app`.)
4. Eject, then open **Applications** on the device and launch **inkshelf**.

## Testing

The parsing and server logic is covered by a host test gate that needs
**neither the PocketBook SDK nor a network connection** — it generates shim
`inkview.h` / libcurl headers so the repo stays self-contained:

```bash
tests/run_host_tests.sh
```

It runs, under AddressSanitizer + UBSan:

- unit tests for the dependency-free OPDS parsing code (`xml.c` + `opds.c`),
- unit tests for the upload HTTP server (`httpd.c`),
- an integration smoke test that drives the whole app (catalog → browse →
  search → book detail and back) against stub InkView + libcurl.

## Layout

```
src/
  main.c            event-loop entry point
  app.{c,h}         screen nav-stack (push/pop/repaint, key/pointer dispatch)
  ui.{c,h}          fonts, header/footer chrome, scrollable list widget
  screens.{c,h}     main menu + screen wiring
  opds.{c,h}        OPDS Atom feed model + link/entry classification
  xml.{c,h}         dependency-free SAX-style XML parser
  http.{c,h}        libcurl HTTP fetch helpers
  opds_ui.c         OPDS browser UI (catalog picker, browse, search, detail)
  download.{c,h}    book download to the device library
  library.{c,h}     library paths + PocketBook library rescan
  httpd.{c,h}       WiFi-drop embedded HTTP upload server
cmake/              arm-obreey cross-compile toolchain file
tests/              host test gate (no SDK / no network)
build.sh            Docker / direct build wrapper
```

## License

TBD.
