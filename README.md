# roon-display

Experimental native Roon Zone Display client in C++20. It discovers Roon without
a configured IP, registers the observed Roon Web Display services, and exposes one
display candidate.

This project is not affiliated with or supported by Roon Labs. The Zone Display
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


## Optional native desktop shell

When GTK3 and WebKitGTK development packages are installed, CMake also builds
`native-shell`. It embeds the shared UI in a native window:

```sh
./build/native-shell http://127.0.0.1:8787/ui/
```

Start `live-preview` first. Configure with
`-DROON_DISPLAY_BUILD_NATIVE_SHELL=OFF` to suppress dependency detection.

## Flatpak packaging

The Flatpak manifest is in `packaging/flatpak/com.julianknepel.roon-display.yml`.
Install Flatpak tooling and the GNOME runtime/SDK, then build and run:

```sh
sudo apt install flatpak flatpak-builder
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Sdk//46 org.gnome.Platform//46
flatpak-builder --user --install --force-clean build-flatpak packaging/flatpak/com.julianknepel.roon-display.yml
flatpak run com.julianknepel.roon-display
```

The bundle grants LAN access for Roon discovery and starts the preview server and
native WebKit shell together as one desktop application.
