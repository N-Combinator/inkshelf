# inkshelf

Native PocketBook OPDS browser + WiFi book drop — **no KOReader, no jailbreak**.

inkshelf is a single `.app` built against the official PocketBook InkView SDK.
It runs on stock firmware: copy `inkshelf.app` onto the SD card and launch it
from the device's application menu.

## What it does

- **OPDS catalog browser** — point it at any OPDS feed, browse, search and
  download books straight into the native PocketBook library. Ships with
  presets for [Standard Ebooks](https://standardebooks.org) and
  [Project Gutenberg](https://www.gutenberg.org), plus a custom-URL entry via
  the on-screen keyboard.
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
1. Pick a preset (Standard Ebooks, Project Gutenberg) or **Custom URL...** and
   type an OPDS feed address with the on-screen keyboard.
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
official [PocketBook SDK_6.3.0](https://github.com/pocketbook/SDK_6.3.0). The
SDK toolchain + InkView sysroot are most easily obtained via the SDK Docker
image.

```bash
# Build inside the SDK Docker image (default).
./build.sh

# Or, if the toolchain + sysroot are already on PATH / in a sysroot:
PB_SDK_ROOT=/path/to/sdk NO_DOCKER=1 ./build.sh
```

The artifact is `build/inkshelf.app`.

### Manual CMake invocation

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-obreey.cmake \
  -DPB_SDK_ROOT=/path/to/sdk
cmake --build build --parallel
```

## Installing (no jailbreak)

1. Connect the PocketBook to your computer over USB, or pull its SD card.
2. Copy `build/inkshelf.app` to the `applications/` folder on the SD card.
3. Eject, then open **Applications** on the device and launch **inkshelf**.

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
