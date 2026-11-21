# Changes — MeshArg vs upstream Meshtastic firmware

MeshArg is a fork of [meshtastic/firmware](https://github.com/meshtastic/firmware). All modifications
are licensed under **GPL-3.0** (same as upstream). This file documents what differs, per GPL-3.0 §5(a)
(stating changes). Stable base: upstream tag `v2.7.26.54e0d8d` (commit `54e0d8d0a`).
Compare this branch with the original release using `git diff v2.7.26.54e0d8d..HEAD`.

## Modules

### ReplyBotModule — expanded into a full auto-responder
`src/modules/ReplyBotModule.{cpp,h}` — upstream ships a ~180-line example reply bot that is excluded
from builds by default; MeshArg expands it to a ~480-line auto-responder. It listens for text "slash
commands" broadcast on the secondary channel named exactly **ambaBOTs**. It ignores DMs and other channels, and replies on the bot
channel as a **threaded reply**:

- `/ping` `/hello` `/test` — "mic check": reports the request's hops-away, RSSI, and SNR.
- bare `test` / `ping` (whole message, case-insensitive) — same mic check.
- `/net` — live mesh health: online/total nodes, channel utilization, TX airtime, noise floor.
  Aliases: `/info`, and the bare whole-words `net` / `info`.
- `/node` — this device: firmware version + `MeshArg MOD` tag, battery/USB, uptime.
- `/route` — fires a traceroute back to the requester; sends an immediate "trace started" ack, then
  replies with the node-by-node path (long/short names + node IDs + per-hop SNR), split into two
  messages if it would exceed the 233-byte text limit; replies "no response received" on timeout.
- `/help` — lists the commands.

Notes: a per-sender cooldown mechanism is retained but currently disabled (responds immediately).
Enabled at build time via `platformio.ini` (below).

### TraceRouteModule — result observer + per-call timeout
`src/modules/TraceRouteModule.{cpp,h}`:
- Added a **result-observer callback** (`setResultObserver`) so another module (ReplyBot) is notified
  when a trace it started completes or times out — receiving the raw `RouteDiscovery` on success or a
  fallback string on timeout.
- Added an optional **per-call tracking timeout**: `startTraceRoute(node, timeoutMs = 0, channel = 0)`, resolved
  into `activeTrackingTimeoutMs` at the TRACKING transition. ReplyBot's `/route` passes 30 s and the bot channel index; the
  on-device menu (`launch()` and the node-picker) keeps the stock 10 s and never inherits the bot's
  value.

## Build configuration
`platformio.ini` — enables ReplyBotModule by removing the `MESHTASTIC_EXCLUDE_REPLYBOT=1` exclusion
from the default build flags.

## Compatibility
App-layer only — **no changes to packet format, routing, or encryption**. MeshArg nodes interoperate
with stock Meshtastic nodes on the same major firmware version.

## Commits (ported from the mesharg branch)
- `35e8a0ed1` → `5630a0ba4` — TraceRouteModule: add result observer + per-call tracking-timeout override
- `051bbdc06` → `f2dc03455` — ReplyBotModule: auto-responder (commands, on-channel threaded replies, bot-only 30 s `/route` timeout)
