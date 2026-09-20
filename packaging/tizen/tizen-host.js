(function () {
  "use strict";
  const artwork = new Map();
  const callbacks = new Set();
  const worker = new Worker("tizen-worker.js");
  let latestState = { activeZone: null };
  function uuid() {
    if (crypto && crypto.randomUUID) return crypto.randomUUID();
    const bytes = new Uint8Array(16); crypto.getRandomValues(bytes);
    bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
    return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("").replace(/(.{8})(.{4})(.{4})(.{4})/, "$1-$2-$3-$4-");
  }
  let displayKey;
  try { displayKey = localStorage.getItem("roon-display.tizen.key"); if (!displayKey) { displayKey = uuid(); localStorage.setItem("roon-display.tizen.key", displayKey); } } catch (_) { displayKey = uuid(); }
  function hydrate(state) {
    const now = state && state.activeZone && state.activeZone.nowPlaying;
    if (now && now.imageKey && artwork.has(now.imageKey)) now.artworkUrl = artwork.get(now.imageKey);
    if (now && Array.isArray(now.artistImageUrls)) now.artistImageUrls = now.artistImageUrls.map((url) => artwork.get(url.slice("/artwork/".length)) || url);
    return state;
  }
  function publish(state) { latestState = hydrate(state); callbacks.forEach((callback) => callback(latestState)); }
  worker.addEventListener("message", (event) => {
    const message = event.data || {};
    if (message.type === "state") publish(message.state);
    if (message.type === "artwork") { const old = artwork.get(message.key); if (old) URL.revokeObjectURL(old); artwork.set(message.key, URL.createObjectURL(new Blob([message.bytes], { type: message.mime }))); publish(latestState); }
    if (message.type === "error") console.error("Roon Display:", message.message);
  });
  worker.postMessage({ type: "start", name: "Samsung TV", displayKey });
  window.RoonDisplayHost = {
    subscribe(onState) { callbacks.add(onState); onState(latestState); return () => callbacks.delete(onState); },
    control(request) { worker.postMessage({ type: "control", command: request.command, position: request.position }); return Promise.resolve(); }
  };
  const mediaKeys = ["MediaPlayPause", "MediaPlay", "MediaPause", "MediaTrackPrevious", "MediaTrackNext"];
  if (window.tizen && tizen.tvinputdevice) { try { tizen.tvinputdevice.registerKeyBatch(mediaKeys); } catch (_) { mediaKeys.forEach((key) => { try { tizen.tvinputdevice.registerKey(key); } catch (_) {} }); } }
  document.addEventListener("keydown", (event) => {
    const code = event.keyCode;
    const command = code === 10252 ? "playpause" : code === 415 ? "play" : code === 19 ? "pause" : code === 10232 ? "previous" : code === 10233 ? "next" : null;
    if (command) { event.preventDefault(); window.RoonDisplayHost.control({ command }); }
  });
  window.addEventListener("beforeunload", () => { worker.postMessage({ type: "stop" }); artwork.forEach((url) => URL.revokeObjectURL(url)); });
})();
