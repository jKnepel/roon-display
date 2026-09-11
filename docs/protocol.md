# Protocol investigation notes

## Verified from Roon's open-source API

1. Discovery is SOOD v2 over UDP port `9003`, sent to `239.255.90.90` and each
   interface's directed broadcast address.
2. The Core query service ID is `00720724-5143-4a9b-abac-0e50cba674bb`.
3. Replies provide `unique_id` and `http_port`.
4. The API connection is `ws://host:http_port/api`.
5. Each binary WebSocket message contains one LF-delimited MOO/1 message.
6. Registration calls registry `info` and then `register`.

Primary references:

- <https://github.com/RoonLabs/node-roon-api/blob/master/sood.js>
- <https://github.com/RoonLabs/node-roon-api/blob/master/moo.js>
- <https://github.com/RoonLabs/node-roon-api/blob/master/moomsg.js>
- <https://github.com/RoonLabs/node-roon-api/blob/master/transport-websocket.js>
- <https://github.com/RoonLabs/node-roon-api/blob/master/lib.js>

## Observed but unpublished

Web Display logs establish extension ID `com.roonlabs.display_zone`, provided
services `zonedisplay:1`, `pairing:1`, and `ping:1`, followed by a Core request to
`subscribe_displays`. An observed display has `display_key`, `auto_name`, and
nullable `active_zone_id`. The prototype therefore answers `CONTINUE Subscribed`
with the smallest observed shape:

```json
{"displays":[{"display_key":"persistent UUID","auto_name":"name","active_zone_id":null}]}
```

There is no public Zone Display schema. The exact response name, optional fields,
settings semantics, and content methods require a live Roon Server capture.

## First live test

1. Run with tracing enabled and save stdout/stderr.
2. Verify discovery, registry `info`, and `register` succeed.
3. Inspect the complete `subscribe_displays` request.
4. Check whether the display appears in Roon's UI.
5. Assign it to a zone and capture every resulting request, including rejected ones.

Traces may contain LAN addresses, Core IDs, tokens, zone IDs, and track metadata;
redact those before publishing.

Web Display knows its Core because its page came from that Core. This prototype uses
extension-style SOOD discovery to meet the no-configured-address goal. It remains to
prove that current Roon accepts a separately discovered client using Web Display's
reserved extension ID. That assumption is localized in the client options.

