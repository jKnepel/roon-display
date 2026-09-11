/*
 * Shared display UI contract.
 * Hosts should call window.RoonDisplay.update(DisplayState). The UI does not
 * know about MOO, SOOD, WebSockets, Android, or Tizen.
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const host = window.RoonDisplayHost || {};
  let currentArtworkUrl = null;
  let progressPosition = 0;
  let progressLength = 0;
  let progressState = "stopped";
  let progressUpdatedAt = 0;
  const elements = {
    zone: $("zone"), state: $("state"), track: $("track"), artist: $("artist"),
    album: $("album"), progress: $("progress"), elapsed: $("elapsed"),
    duration: $("duration"), previous: $("previous"), playPause: $("play-pause"), next: $("next"), artwork: $("artwork"), connection: $("connection"),
    controlStatus: $("control-status"),
  };

  async function command(command, position) {
    elements.controlStatus.textContent = command === "seek" ? "Seeking…" : "Sending…";
    try {
      const request = position === undefined ? { command } : { command, position };
      if (typeof host.control === "function") {
        await host.control(request);
        elements.controlStatus.textContent = "";
        return;
      }
      const response = await fetch("/control", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request) });
      if (!response.ok) { elements.controlStatus.textContent = "Unavailable"; console.warn("Roon control rejected:", command); } else { elements.controlStatus.textContent = ""; }
    } catch (error) { elements.controlStatus.textContent = "Control failed"; console.error("Roon control:", error); }
  }

  function clock(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return "0:00";
    const minutes = Math.floor(seconds / 60);
    return `${minutes}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;
  }

  function update(state) {
    window.__roonDisplayState = state;
    progressUpdatedAt = performance.now();
    const zone = state && state.activeZone ? state.activeZone : null;
    if (!zone) {
      elements.zone.textContent = "Waiting for zone";
      elements.state.textContent = "STOPPED";
      elements.track.textContent = "No track playing";
      elements.artist.textContent = "—";
      elements.album.textContent = "—";
      elements.progress.style.width = "0%";
      elements.previous.disabled = elements.playPause.disabled = elements.next.disabled = true;
      document.querySelector(".timeline").classList.add("disabled");
      return;
    }
    const now = zone.nowPlaying || {};
    const lines = now.twoLine || {};
    const detailed = now.threeLine || {};
    elements.zone.textContent = zone.displayName || "Roon zone";
    elements.state.textContent = String(zone.state || "stopped").toUpperCase();
    elements.playPause.textContent = zone.state === "playing" ? "⏸" : "▶";
    elements.playPause.setAttribute("aria-label", zone.state === "playing" ? "Pause" : "Play");
    elements.previous.disabled = zone.previousAllowed === false;
    elements.next.disabled = zone.nextAllowed === false;
    elements.playPause.disabled = zone.state === "playing" ? zone.pauseAllowed === false : zone.playAllowed === false;
    document.querySelector(".timeline").classList.toggle("disabled", zone.seekAllowed === false);
    elements.track.textContent = detailed.line1 || lines.line1 || now.track || "No track playing";
    elements.artist.textContent = detailed.line2 || lines.line2 || now.artist || "—";
    elements.album.textContent = detailed.line3 || now.album || "—";
    const position = Number(now.seekPosition ?? zone.seekPosition ?? 0);
    const duration = Number(now.length ?? 0);
    progressPosition = position; progressLength = duration; progressState = zone.state || "stopped";
    elements.elapsed.textContent = clock(position);
    elements.duration.textContent = clock(duration);
    elements.progress.style.width = duration > 0 ? `${Math.min(100, Math.max(0, position / duration * 100))}%` : "0%";
    if (now.imageKey) {
      // Artwork retrieval is host-specific. The key is exposed as a CSS hook
      // now; a host can replace this with a blob/data URL later.
      elements.artwork.dataset.imageKey = now.imageKey;
      if (now.artworkUrl && now.artworkUrl !== currentArtworkUrl) { elements.artwork.style.backgroundImage = `url("${now.artworkUrl}")`; currentArtworkUrl = now.artworkUrl; }
    }
    elements.connection.classList.add("connected");
  }

  function tickProgress() {
    if (progressState !== "playing" || progressLength <= 0) return;
    const position = Math.min(progressLength, progressPosition + (performance.now() - progressUpdatedAt) / 1000);
    elements.elapsed.textContent = clock(position);
    elements.progress.style.width = `${Math.min(100, position / progressLength * 100)}%`;
  }
  window.setInterval(tickProgress, 1000);

  window.RoonDisplay = { update };
  elements.previous.addEventListener("click", () => command("previous"));
  elements.next.addEventListener("click", () => command("next"));
  elements.playPause.addEventListener("click", () => command("playpause"));
  document.querySelector(".timeline").addEventListener("click", (event) => {
    const duration = Number((window.__roonDisplayState || {}).activeZone?.nowPlaying?.length || 0);
    if (duration <= 0) return;
    if (event.currentTarget.classList.contains("disabled")) return;
    const bounds = event.currentTarget.getBoundingClientRect();
    const ratio = Math.min(1, Math.max(0, (event.clientX - bounds.left) / bounds.width));
    command("seek", duration * ratio);
  });

  // Fixture mode: load the same shape as the checked-in transport examples,
  // normalize it to the UI contract, and replay the state transition once.
  async function fixtureMode() {
    if (!location.search.includes("fixture")) {
      if (typeof host.subscribe === "function") {
        host.subscribe(update, () => elements.connection.classList.remove("connected"));
        return;
      }
      const events = new EventSource("/events");
      events.onmessage = (event) => { try { update(JSON.parse(event.data)); } catch (error) { console.error("state event:", error); } };
      events.onerror = () => elements.connection.classList.remove("connected");
      return;
    }
    const [initial, changed] = await Promise.all([
      fetch("../fixtures/transport/subscribed.json").then((r) => r.json()),
      fetch("../fixtures/transport/changed.json").then((r) => r.json())
    ]);
    const toState = (payload) => {
      const source = (payload.zones || payload.zones_changed || [])[0];
      if (!source) return {};
      const now = source.now_playing || {};
      return { activeZone: { displayName: source.display_name, state: source.state,
        seekPosition: source.seek_position, nowPlaying: { ...now,
          seekPosition: now.seek_position, twoLine: now.two_line, threeLine: now.three_line } } };
    };
    update(toState(initial));
    window.setTimeout(() => update(toState(changed)), 3500);
  }
  fixtureMode().catch((error) => console.error("fixture mode:", error));
})();
