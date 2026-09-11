# Shared display UI

This is a platform-independant full-screen Now Playing view. It can be opened in a
browser and embedded in Linux, Android TV, or Tizen hosts via the build deployment.

Run fixture mode from the project root:

```sh
python3 -m http.server 8080
```

Open <http://localhost:8080/ui/?fixture>. The page loads the checked-in transport
fixtures, renders the initial playing state, then replays the paused update after
3.5 seconds.

Host integration is intentionally small:

```js
window.RoonDisplay.update({
  activeZone: {
    displayName: "Living Room",
    state: "playing",
    nowPlaying: {
      artist: "Artist",
      album: "Album",
      track: "Track",
      seekPosition: 12,
      length: 240,
      imageKey: "album-art-key",
      twoLine: { line1: "Artist", line2: "Track" }
    }
  }
});
```

The host owns Roon protocol and artwork retrieval. The UI only consumes the
normalized display-state contract.

## Native host adapter

A native host may define `window.RoonDisplayHost` before loading `app.js`:

```js
window.RoonDisplayHost = {
  subscribe(onState, onDisconnect) {
    // Call onState(normalizedDisplayState) for each update.
  },
  control(request) {
    // request is { command: "playpause" | "previous" | "next" }
    // or { command: "seek", position: seconds }.
  }
};
```

When this object is absent, the Linux preview fallback uses `/events` and
`POST /control`. This keeps the UI reusable by Tizen and Android hosts.

The desktop preview loads `hosts/http.js`, an HTTP host adapter that maps the
shared callbacks to `/events` and `POST /control`. Native hosts should provide
the same `RoonDisplayHost` object before loading `app.js`.
