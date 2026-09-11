(function () {
  "use strict";
  const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.roonDisplay;
  const native = location.protocol === "roon-display:";
  if (bridge || native) {
    let nextId = 1;
    const pending = new Map();
    window.__roonDisplayControlResult = (id, ok) => {
      const result = pending.get(id);
      if (!result) return;
      pending.delete(id);
      if (ok) result.resolve();
      else result.reject(new Error("control rejected"));
    };
    window.RoonDisplayHost = {
      subscribe(onState) {
        window.__roonDisplayNativeState = onState;
        if (bridge) bridge.postMessage(JSON.stringify({ type: "ready" }));
        return () => { window.__roonDisplayNativeState = null; };
      },
      control(request) {
        if (!bridge) return Promise.reject(new Error("native bridge unavailable"));
        const id = nextId++;
        return new Promise((resolve, reject) => {
          pending.set(id, { resolve, reject });
          bridge.postMessage(JSON.stringify({ type: "control", id, request: JSON.stringify(request) }));
        });
      }
    };
    window.RoonDisplayNativeState = (state) => {
      if (window.__roonDisplayNativeState) window.__roonDisplayNativeState(state);
    };
    return;
  }
  window.RoonDisplayHost = {
    subscribe(onState, onDisconnect) {
      let stopped = false;
      const poll = () => fetch("/state", { cache: "no-store" })
        .then((response) => { if (!response.ok) throw new Error(`state rejected (${response.status})`); return response.json(); })
        .then((state) => { if (!stopped) onState(state); })
        .catch((error) => { if (!stopped) { console.error("state poll:", error); if (onDisconnect) onDisconnect(); } });
      poll();
      const timer = window.setInterval(poll, 250);
      return () => { stopped = true; window.clearInterval(timer); };
    },
    async control(request) {
      const response = await fetch("/control", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request) });
      if (!response.ok) throw new Error(`control rejected (${response.status})`);
      return response.json();
    }
  };
})();
