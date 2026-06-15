# inkshelf

Native PocketBook OPDS browser + WiFi book drop — **no KOReader, no jailbreak**.

inkshelf is a single `.app` built against the official PocketBook InkView SDK.
It runs on stock firmware: copy `inkshelf.app` onto the SD card and launch it
from the device's application menu.

## What it does

- **OPDS catalog browser** — point it at any OPDS feed, search, browse and
  download books straight onto the device library.
- **WiFi book drop** — starts a tiny HTTP server on the reader; from a PC or
  phone on the same WiFi you open the shown URL and upload `epub`/`fb2` files
  that then appear in the native PocketBook library.
- **Minimal e-ink UI** drawn directly with the InkView API.

> Status: early development. The scaffold (this milestone) brings up the
> InkView event loop and a splash screen; OPDS and WiFi-drop land in following
> milestones. See the [roadmap board](https://github.com/orgs/N-Combinator/projects/8).

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

## Layout

```
src/        application sources (entry point, UI, OPDS, WiFi drop)
cmake/      arm-obreey cross-compile toolchain file
build.sh    Docker / direct build wrapper
```

## License

TBD.
