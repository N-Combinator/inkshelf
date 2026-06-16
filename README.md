# inkshelf

Native PocketBook OPDS browser + WiFi book drop — **no KOReader, no jailbreak**.

inkshelf is a single `.app` built against the official PocketBook InkView SDK.
It runs on stock firmware: copy `inkshelf.app` onto the SD card and launch it
from the device's application menu.

## What it does

- **OPDS catalog browser** — point it at any OPDS feed, browse, filter and
  download books straight into the native PocketBook library. Ships with
  presets for [Project Gutenberg](https://www.gutenberg.org) and
  [Flibusta](https://flibusta.is/opds) (Russian-language), plus a custom-URL
  entry via the on-screen keyboard. Every list has a tap-to-filter bar (by
  title/author) and a scrollbar for long catalogs.
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
1. Pick a preset (Project Gutenberg, Flibusta) or **Custom URL...** and type an
   OPDS feed address with the on-screen keyboard.
2. Browse the feed; nested catalogs open inline, book entries open a detail
   view. Tap the filter bar at the top of any list to narrow it by title or
   author; the **Menu** key runs the catalog's own server-side search where the
   feed advertises one.
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

`inkshelf-build.sh` (repo root) does the whole loop in one shot: cmake configure
→ cross-compile for ARM → verify the output is an ARM ELF → copy `inkshelf.app`
onto a USB-connected reader.

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
Docker not running, image absent) instead of failing obscurely. The artifact is
`build/inkshelf.app`.

### HTTPS and TLS verification

PocketBook firmware's libcurl is built against **NSS**, not OpenSSL. NSS ignores
`CURLOPT_CAINFO` pointed at a PEM bundle (it expects an NSS certificate
database), so supplying a CA file always failed the handshake with
`CURLE_SSL_CACERT_BADFILE` (curl error 77). inkshelf therefore disables TLS
peer/host verification (`CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` = 0) instead of
shipping an NSS trust DB. That is an accepted trade-off for this app: it only
fetches public OPDS feeds and public-domain books and never sends credentials
or writes data, so there is nothing for a man-in-the-middle to steal. No CA
bundle needs to be installed on the device.

### Manual CMake invocation

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-obreey.cmake \
  -DPB_SDK_ROOT=/path/to/sdk
cmake --build build --parallel
```

## Installing (no jailbreak)

1. Connect the PocketBook to your computer over USB, or pull its SD card.
2. Copy `build/inkshelf.app` into the `applications/` folder on the device.
3. Eject, then open **Applications** on the device and launch **inkshelf**.

No CA bundle or extra files are needed (see *HTTPS and TLS verification* above).

## Wireless deploy (developer loop — needs PBJB)

For the USB-free edit→build→test loop, the `Makefile` can push a freshly built
`build/inkshelf.app` to a reader over WiFi. **This path requires the
[PBJB](https://github.com/SquidDev/pbjb) jailbreak** (it opens SSH/netcat on the
device); plain end-user installs stay USB-only and jailbreak-free, as above.

```bash
make build                       # cross-compile (same as ./build.sh; needs the SDK)
make test                        # host test gate (no SDK / no device)
make deploy    DEVICE=<reader-ip># scp over SSH  (primary)
make deploy-nc DEVICE=<reader-ip># netcat receiver.app  (no root needed)
```

**`make deploy`** copies the `.app` over SSH into `/mnt/ext1/applications/`.
PBJB's dropbear is old, so the recipe re-enables `ssh-rsa` for both the host key
and pubkey auth (modern OpenSSH disables it by default). A running `inkshelf.app`
holds its ELF open, so writing straight over it would fail with `ETXTBSY`;
instead it uploads to `inkshelf.app.new`, stops any running instance, then
renames over the target — safe even while the app is on screen. Overrides:

```bash
make deploy DEVICE=192.168.1.42 PORT=2468 SSH_USER=root   # dropbear sometimes listens on 2468
```

**`make deploy-nc`** is the no-root alternative: it talks to `receiver.app` (a
small `nc -l` shell script living on the device, pattern from the
[PocketBook cheatsheet](https://blog.flxzt.net/posts/pb-cheatsheet/)) — sends the
filename, then the binary, on port `19991` (`NC_PORT=` to change). If the binary
transfer hangs because your host `nc` doesn't close on EOF, add `-N` (BSD nc) or
`-q0` (traditional) to the second `nc` in the recipe.

After `make deploy`/`deploy-nc`, relaunch inkshelf from the device's
application list.

### Over-the-air: the built-in `/deploy` endpoint

inkshelf's WiFi-drop server also accepts the running binary itself. While the
**WiFi Book Drop** screen is open, `POST /deploy` with the new `.app` as a
multipart file overwrites `/mnt/ext1/applications/inkshelf.app` and restarts the
app — **no SSH, no jailbreak, no cable**. The write is atomic (`.part` +
`rename`, safe even though the app is overwriting its own running binary) and
guarded: the part must be `application/octet-stream` and ≤ 10 MB.

The repo ships `inkshelf-deploy-wifi.sh` for this:

```bash
./inkshelf-deploy-wifi.sh 192.168.1.42        # HTTP POST to /deploy (app must be on the WiFi-drop screen)
./inkshelf-deploy-wifi.sh 192.168.1.42 --ssh  # SCP fallback (needs sshd/PBJB)
./inkshelf-deploy-wifi.sh --find              # discover the reader's IP on the LAN
```

> **Security note:** `/deploy` runs arbitrary uploaded code on the device. The
> attack surface is bounded — the server only listens while you have the WiFi
> Book Drop screen open — but treat it as a trusted-LAN developer convenience,
> not something to leave exposed. The mime/size checks are sanity gates, not
> authentication.

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
Makefile            build/test + wireless deploy (make deploy / deploy-nc)
inkshelf-deploy-wifi.sh  host helper: OTA deploy via /deploy endpoint or scp
```

## License

TBD.
