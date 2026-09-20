# roon-display

Experimental native Roon Zone Display client in C++20. This app displays Roon´s Now Playing information
by advertising itself as a display to the Roon Web Display services.

This project is not affiliated with or supported by Roon Labs. The Roon Web Display
service is unpublished and may change between releases.

## Build

The prototype has no third-party runtime dependencies and currently targets Linux.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

Run it on the same IPv4 LAN as Roon Server:

```sh
./build/roon-display --name "Test Roon Display"
```

It uses SOOD multicast and directed broadcast, connects to the discovered
`ws://SERVER:PORT/api`, registers the observed services, and responds to
`subscribe_displays`. Wire tracing is on by default; `--quiet` disables it.

Expected progress:

```text
Roon Core discovered at 192.168.1.10:9330
Roon Core connected: Roon Server
Display service registered; waiting for subscribe_displays
Display announced to Roon: Test Roon Display (...)
```

The final line proves that the Core subscribed and received the announcement. It
does not yet prove that Roon's UI accepted it. Check the Display picker and retain
the trace if it does not appear.

- `--name NAME` changes the display name.
- `--display-key UUID` supplies a persistent identity; the default derives from
  the host name.
- `--quiet` disables MOO tracing.

Implemented: SOOD, WebSocket transport, MOO/1 framing, registry registration,
ping/pairing basics, and the initial Zone Display subscription. Token persistence,
reconnect, content messages, rendering, and Tizen support remain future work.

See [docs/protocol.md](docs/protocol.md) for evidence boundaries and capture steps.


## Browser preview

The development UI is hosted by `live-preview` and opened directly in a
browser:

```sh
./build/live-preview 8787 .
# open http://127.0.0.1:8787/ui/
```

## Flatpak packaging

The Flatpak manifest is in `packaging/flatpak/com.julianknepel.roon-display.yml`.
Install Flatpak tooling and the GNOME runtime/SDK, then build and run:

```sh
sudo apt install flatpak flatpak-builder
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Sdk//50 org.gnome.Platform//50
flatpak-builder --user --install --force-clean build-flatpak packaging/flatpak/com.julianknepel.roon-display.yml
flatpak run com.julianknepel.roon-display
```

The bundle grants LAN access for Roon discovery and launches the native WebKit
desktop shell. The desktop executable serves its UI through an in-process
`roon-display://` scheme and communicates with the UI through a native WebKit
bridge.

## Samsung Tizen TV

The Samsung target packages the shared UI with the C++ Roon client compiled to
WebAssembly. It requires a Samsung TV running Tizen 5.5 or newer, Samsung's
Emscripten SDK with its Tizen sockets extension, and the Tizen Studio CLI.

Build the staged TV Web App:

```sh
scripts/build-tizen.sh
```

Create a signed WGT for direct Tizen installation with a local certificate
profile:

```sh
scripts/package-tizen.sh YOUR_CERTIFICATE_PROFILE
```

The result is written to `dist/tizen/`. When distributing a GitHub release for
Apps2Samsung, publish the WGT and let Apps2Samsung re-sign it for each TV. Keep
the `RDSPY12345.RoonDisplay` app ID stable and increment the version in
`packaging/tizen/config.xml` for updates.

The TV host persists a display UUID in Web Storage, runs Roon discovery and
transport in a WebAssembly worker, forwards artwork as Blob URLs, and maps
Samsung media keys to the shared playback controls. Samsung's socket extension
must be validated on the actual target LAN because Tizen cannot enumerate
interfaces for the desktop directed-broadcast fallback.
