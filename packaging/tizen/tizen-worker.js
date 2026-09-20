/* Runs Roon's raw TCP/UDP sockets away from the Tizen Web UI thread. */
let runtime;
self.addEventListener("message", async (event) => {
  const message = event.data || {};
  // Forward messages emitted by the proxied WASM pthread to the UI worker owner.
  if (message.type === "state" || message.type === "artwork" || message.type === "error") {
    self.postMessage(message, message.type === "artwork" ? [message.bytes] : []);
    return;
  }
  if (message.type === "start") {
    try {
      importScripts("roon-display-tizen.js");
      runtime = await createRoonDisplayModule({ arguments: ["--name", message.name, "--display-key", message.displayKey] });
      self.postMessage({ type: "ready" });
    } catch (error) {
      self.postMessage({ type: "error", message: String(error && error.message || error) });
    }
  } else if (message.type === "control" && runtime) {
    if (message.command === "seek") runtime.ccall("roon_display_seek", null, ["number"], [message.position]);
    else runtime.ccall("roon_display_control", null, ["string"], [message.command]);
  } else if (message.type === "stop" && runtime) {
    runtime.ccall("roon_display_stop", null, [], []);
  }
});
