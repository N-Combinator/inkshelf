# Building inkshelf from source

**Most people do not need this page.** Every release ships a prebuilt
`inkshelf.app`; see [Install](README.md#install). Build from source if you want
to change the code, or to check what you are installing.

inkshelf cross-compiles with the `arm-obreey-linux-gnueabi` toolchain from the
official [PocketBook SDK_6.3.0](https://github.com/pocketbook/SDK_6.3.0). The
output is always a single `build/inkshelf.app` (an ARM 32-bit ELF).

**Contents**

- [Get the SDK](#1-get-the-sdk-on-the-65-branch-not-master)
- [One-command build](#2a-one-command-build-recommended-linux-x86_64)
- [Wireless build & deploy](#2b-wireless-build--deploy)
- [Manual CMake](#2c-manual-cmake)
- [Installing over USB](#installing-over-usb-no-jailbreak)
- [A note on HTTPS / TLS](#a-note-on-https--tls)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Cutting a release](#cutting-a-release)

> **TL;DR** — on a Linux x86_64 host with the SDK in place:
> `./inkshelf-build.sh` builds and copies over USB, or
> `./inkshelf-build-wifi.sh --find --pull --pin <PIN>` builds and deploys over WiFi.

## 1. Get the SDK (on the `6.5` branch, not `master`)

The repo's default `master` branch contains **only a README** — the actual SDK
lives on the `6.5` branch under `SDK-B288/`.

```bash
git clone --depth 1 --single-branch --branch 6.5 \
  https://github.com/pocketbook/SDK_6.3.0 ~/pocketbook-sdk   # SDK-B288/ is now there
```

(A plain `git clone` pulls ~680 MB because it downloads every branch's objects;
the flags above fetch only the one branch at one revision.)

The compiler is `SDK-B288/usr/bin/arm-obreey-linux-gnueabi-gcc` and the InkView
sysroot is `SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot`. **The toolchain
binaries are Linux x86_64 ELF** — they run on a Linux x86_64 host only (not
natively on macOS; use a `linux/amd64` container there).

## 2a. One-command build (recommended, Linux x86_64)

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

## 2b. Wireless build & deploy

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

## 2c. Manual CMake

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-obreey.cmake \
  -DPB_SDK_ROOT=/path/to/SDK-B288
cmake --build build --parallel
```

`build.sh` wraps the same thing (and can run it inside your own SDK container
via `USE_DOCKER=1 PB_SDK_IMAGE=...`); it is what CI calls.

Building this way — driving `cmake` yourself rather than through `build.sh` or
`inkshelf-build.sh` — you may hit:

```
cc1: error while loading shared libraries: libmpfr.so.4
```

The SDK's compiler is a 2017 gcc 6.3 that wants mpfr 3.x, and distros have
shipped `libmpfr.so.6` for years with no compatible `.so.4` to install. The SDK
carries the right library itself, so point the loader at it — and only at the
libraries the compiler needs, because `$PB_SDK_ROOT/usr/lib` also holds 2017
builds of glib/icu/expat that would shadow your host's:

```bash
mkdir -p build/.hostlibs
for lib in libmpfr.so.4 libmpc.so.3 libgmp.so.10; do
  ln -sfn "$PB_SDK_ROOT/usr/lib/$lib" "build/.hostlibs/$lib"
done
export LD_LIBRARY_PATH="$PWD/build/.hostlibs:$LD_LIBRARY_PATH"
```

Both build scripts do this for you.

## Installing over USB (no jailbreak)

To install without the build script (a prebuilt `.app`, or after a manual CMake
build), connect the reader over USB or pull its SD card, copy `build/inkshelf.app`
into the `applications/` folder, eject, and launch **inkshelf** from the
Applications menu. (2a does this automatically over USB; 2b does it over WiFi.)

## A note on HTTPS / TLS

PocketBook firmware's libcurl is built against **NSS**, not OpenSSL. NSS ignores
`CURLOPT_CAINFO` pointed at a PEM bundle (it expects an NSS certificate
database), so supplying a CA file always failed the handshake with
`CURLE_SSL_CACERT_BADFILE` (curl error 77). inkshelf therefore disables TLS
peer/host verification (`CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` = 0) rather than
shipping an NSS trust DB. That is an accepted trade-off here: it only fetches
public OPDS feeds and public-domain books and never sends credentials or writes
data, so there is nothing for a man-in-the-middle to steal. No CA bundle needs to
be installed on the device.

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

The same gate runs in CI before any release binary is built.

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
.github/workflows/        CI: test + cross-compile + publish the release binary
```

## Cutting a release

`.github/workflows/release.yml` does the whole thing on a `v*` tag — nobody has
to build a release binary by hand:

```bash
git tag v1.2.0
git push origin v1.2.0
```

The workflow then

1. runs the host test gate (a failure here stops the release; no binary ships),
2. fetches the SDK (shallow clone of branch `6.5`, cached between runs and keyed
   on that branch's head commit),
3. cross-compiles via `build.sh`,
4. refuses to publish anything that `file` does not report as an ARM 32-bit ELF,
5. creates the GitHub release if the tag has none yet, and uploads
   `inkshelf.app` plus `inkshelf.app.sha256` to it.

Writing release notes first is fine: if a release for the tag already exists,
the workflow keeps its notes and only attaches the binaries.

The workflow can also be started by hand from the Actions tab (*Run workflow*).
Leave the `tag` input empty to build whatever branch you picked and get the
`.app` as a workflow artifact. Give it a tag and it checks that tag out, builds
it, and attaches the binaries to its release — which is how a tag that was cut
before this workflow existed (or whose release went out without assets) gets its
binary. The branch chosen in *Use workflow from* only decides which version of
the workflow runs; `tag` decides what gets built.

### Versioning

`CMakeLists.txt` stamps the binary with `git describe --tags --match "v*"`, and
the app's home screen shows that string. So the version users see comes from the
tag: tagged builds read `v1.2.0`, builds ahead of a tag read
`v1.2.0-3-gabc1234`, and a build outside a git checkout falls back to `dev`.
Tag before you build a binary you intend to hand to someone.
