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
  let pendingSeekPosition = null;
  let seekRequestSucceeded = false;
  const elements = {
    zone: $("zone"), track: $("track"), artist: $("artist"),
    album: $("album"), progress: $("progress"), elapsed: $("elapsed"),
    duration: $("duration"), previous: $("previous"), playPause: $("play-pause"), next: $("next"), artwork: $("artwork"),
  };

  async function command(command, position) {
    try {
      const request = position === undefined ? { command } : { command, position };
      if (command === "previous" || command === "next") {
        pendingSeekPosition = null;
        seekRequestSucceeded = false;
        const seekTimeline = document.querySelector(".timeline");
        seekTimeline.classList.remove("seeking");
        seekTimeline.dataset.seeking = "false";
      }
      if (command === "seek" && Number.isFinite(position)) { pendingSeekPosition = position; seekRequestSucceeded = false; }
      const current = window.__roonDisplayState;
      if (current && current.activeZone) {
        if (command === "playpause") {
          current.activeZone.state = current.activeZone.state === "playing" ? "paused" : "playing";
          update(current);
        }
      }
      if (typeof host.control === "function") {
        await host.control(request);
        if (command === "seek") seekRequestSucceeded = true;
        return;
      }
      const response = await fetch("/control", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request) });
      if (!response.ok) throw new Error(`control rejected`);
    } catch (error) {
      if (command === "seek") { pendingSeekPosition = null; seekRequestSucceeded = false; const seekTimeline = document.querySelector(".timeline"); seekTimeline.classList.remove("seeking"); seekTimeline.dataset.seeking = "false"; }
      console.error("Roon control:", error);
    }
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

      elements.track.textContent = "No track playing";
      elements.artist.textContent = "—";
      elements.album.textContent = "—";
      if (!timelineLocked()) elements.progress.style.width = "0%";
      elements.previous.disabled = elements.playPause.disabled = elements.next.disabled = true;
      document.querySelector(".timeline").classList.add("disabled");
      return;
    }
    const now = zone.nowPlaying || {};
    const lines = now.twoLine || {};
    const detailed = now.threeLine || {};
    elements.zone.textContent = zone.displayName || "Roon zone";

    elements.playPause.textContent = zone.state === "playing" ? "⏸" : "▶";
    elements.playPause.setAttribute("aria-label", zone.state === "playing" ? "Pause" : "Play");
    elements.previous.disabled = zone.previousAllowed === false;
    elements.next.disabled = zone.nextAllowed === false;
    elements.playPause.disabled = zone.state === "playing" ? zone.pauseAllowed === false : zone.playAllowed === false;
    document.querySelector(".timeline").classList.toggle("disabled", zone.seekAllowed === false);
    elements.track.textContent = detailed.line1 || lines.line1 || now.track || "No track playing";
    elements.artist.textContent = detailed.line2 || lines.line2 || now.artist || "—";
    elements.album.textContent = detailed.line3 || now.album || "—";
    const position = Number(now.seekPosition != null ? now.seekPosition : (zone.seekPosition != null ? zone.seekPosition : 0));
    const duration = Number(now.length != null ? now.length : 0);
    const seekConfirmed = pendingSeekPosition !== null && seekRequestSucceeded && Math.abs(position - pendingSeekPosition) <= 2;
    progressLength = duration;
    progressState = zone.state || "stopped";
    elements.duration.textContent = clock(duration);
    if (!timelineLocked() && (pendingSeekPosition === null || seekConfirmed)) {
      progressPosition = position;
      elements.elapsed.textContent = clock(position);
      if (seekConfirmed) {
        elements.progress.classList.add("instant");
        pendingSeekPosition = null;
        seekRequestSucceeded = false;
        window.requestAnimationFrame(() => elements.progress.classList.remove("instant"));
        timeline.dataset.seeking = "false";
      }
      elements.progress.style.width = duration > 0 ? (Math.min(100, Math.max(0, position / duration * 100)) + "%") : "0%";
    }
    if (now.imageKey) {
      // Artwork retrieval is host-specific. The key is exposed as a CSS hook
      // now; a host can replace this with a blob/data URL later.
      elements.artwork.dataset.imageKey = now.imageKey;
      if (now.artworkUrl && now.artworkUrl !== currentArtworkUrl) { elements.artwork.style.backgroundImage = `url("${now.artworkUrl}")`; currentArtworkUrl = now.artworkUrl; }
    }

  }

  function tickProgress() {
    if (timelineLocked() || progressState !== "playing" || progressLength <= 0) return;
    const position = Math.min(progressLength, progressPosition + (performance.now() - progressUpdatedAt) / 1000);
    elements.elapsed.textContent = clock(position);
    elements.progress.style.width = `${Math.min(100, position / progressLength * 100)}%`;
  }
  window.setInterval(tickProgress, 1000);

  window.RoonDisplay = { update };
  elements.previous.addEventListener("click", () => command("previous"));
  elements.next.addEventListener("click", () => command("next"));
  elements.playPause.addEventListener("click", () => command("playpause"));
  const timeline = document.querySelector(".timeline");
  let dragging = false;
  let dragPointerId = null;
  function timelineLocked() { return timeline.dataset.dragging === "true" || timeline.dataset.seeking === "true"; }

  function timelinePosition(event) {
    const currentState = window.__roonDisplayState || {};
    const activeZone = currentState.activeZone || {};
    const currentNow = activeZone.nowPlaying || {};
    const duration = Number(currentNow.length || 0);
    if (duration <= 0 || timeline.classList.contains("disabled")) return null;
    const bounds = timeline.getBoundingClientRect();
    const ratio = Math.min(1, Math.max(0, (event.clientX - bounds.left) / bounds.width));
    return duration * ratio;
  }

  function previewSeek(position) {
    progressPosition = position;
    progressUpdatedAt = performance.now();
    elements.elapsed.textContent = clock(position);
    if (progressLength > 0) elements.progress.style.width = (Math.min(100, Math.max(0, position / progressLength * 100)) + "%");
  }

  timeline.addEventListener("pointerdown", (event) => {
    const position = timelinePosition(event);
    if (position === null) return;
    dragging = true;
    dragPointerId = event.pointerId;
    timeline.classList.add("dragging");
    timeline.dataset.dragging = "true";
    timeline.setPointerCapture(event.pointerId);
    previewSeek(position);
    event.preventDefault();
  });
  timeline.addEventListener("pointermove", (event) => {
    if (!dragging || event.pointerId !== dragPointerId) return;
    const position = timelinePosition(event);
    if (position !== null) previewSeek(position);
  });
  timeline.addEventListener("pointerup", (event) => {
    if (!dragging || event.pointerId !== dragPointerId) return;
    const position = timelinePosition(event);
    dragging = false;
    dragPointerId = null;
    timeline.classList.remove("dragging");
    timeline.dataset.seeking = "false";
    timeline.dataset.dragging = "false";
    timeline.releasePointerCapture(event.pointerId);
    if (position !== null) { timeline.classList.add("seeking"); timeline.dataset.seeking = "true"; command("seek", position); }
  });
  timeline.addEventListener("pointercancel", (event) => {
    if (!dragging || event.pointerId !== dragPointerId) return;
    dragging = false;
    dragPointerId = null;
    timeline.classList.remove("dragging");
    timeline.dataset.seeking = "false";
    timeline.dataset.dragging = "false";
    timeline.releasePointerCapture(event.pointerId);
    update(window.__roonDisplayState || {});
  });

  // Fixture mode: load the same shape as the checked-in transport examples,
  // normalize it to the UI contract, and replay the state transition once.
  async function fixtureMode() {
    if (!location.search.includes("fixture")) {
      if (typeof host.subscribe === "function") {
        host.subscribe(update, () => {});
        return;
      }
      const events = new EventSource("/events");
      events.onmessage = (event) => { try { update(JSON.parse(event.data)); } catch (error) { console.error("state event:", error); } };
      events.onerror = () => {};
      return;
    }
    elements.previous.disabled = elements.playPause.disabled = elements.next.disabled = true;
    document.querySelector(".timeline").classList.add("disabled");
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
