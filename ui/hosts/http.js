(function () {
  "use strict";
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
      const response = await fetch("/control", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request)
      });
      if (!response.ok) throw new Error(`control rejected (${response.status})`);
      return response.json();
    }
  };
})();
