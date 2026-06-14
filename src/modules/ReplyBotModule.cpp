#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
/*
 * ReplyBotModule.cpp
 *
 * A simple reply bot for the Meshtastic firmware.  It listens for text "slash commands"
 * delivered either via a direct message (DM) or a broadcast on any configured channel,
 * and auto-replies where the request arrived (on that same channel, or by DM for a DM).
 * Supported commands:
 *   /ping /hello /test  - "mic check": reports hops/RSSI/SNR of the request
 *                         (also triggered by the bare whole-words "test" / "ping")
 *   /net                - live mesh health: online/total nodes, channel util, TX air, noise
 *                         (aliases: /info, and the bare whole-words "net" / "info")
 *   /node               - this device's health: firmware, battery, uptime
 *   /route              - traceroute the path to/from the requester (async; threaded reply)
 *   /help               - lists the available commands
 * Per-sender rate limiting is currently disabled (it responds immediately); the cooldown
 * machinery is retained (see REPLYBOT_*_COOLDOWN_MS) to re-enable in future versions.
 * Enabled by leaving MESHTASTIC_EXCLUDE_REPLYBOT undefined (see platformio.ini).
 */

#include "MeshService.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "ReplyBotModule.h"
#include "TraceRouteModule.h"
#include "airtime.h"
#include "mesh/MeshTypes.h"
#include "mesh/RadioLibInterface.h"

#include <Arduino.h>
#include <cctype>
#include <cstdint>
#include <cstring>

//
// Rate limiting data structures
//
// Each sender is tracked in a small ring buffer.  When a message arrives from a
// sender we check the last time we responded to them.  If the difference is
// less than the configured cooldown (different values for DM vs broadcast)
// the message is ignored; otherwise we update the last response time and
// proceed with replying.

struct ReplyBotCooldownEntry {
    uint32_t from = 0;
    uint32_t lastMs = 0;
};

static constexpr uint8_t REPLYBOT_COOLDOWN_SLOTS = 8;          // ring buffer size
// Cooldowns disabled for now — respond immediately to every command.
// Restore (e.g. 10 * 1000) to re-enable per-sender rate limiting in future versions.
static constexpr uint32_t REPLYBOT_DM_COOLDOWN_MS = 0; // was 10 * 1000 (10 s for DMs)
static constexpr uint32_t REPLYBOT_LF_COOLDOWN_MS = 0; // was 10 * 1000 (10 s for broadcasts)

// /route waits longer than the 10 s on-device-menu default, since multi-hop traces on a busy mesh
// can take a while to round-trip. Only applies to bot-initiated traces (passed into startTraceRoute).
static constexpr uint32_t REPLYBOT_ROUTE_TIMEOUT_MS = 30000;

// Legend appended to the /node reply to mark this as a modified build. Edit to taste.
static constexpr const char *REPLYBOT_MOD_TAG = "MeshArg MOD";

// Meshtastic text payload byte limit — the most a single reply packet can carry.
static constexpr size_t REPLYBOT_MAX_PAYLOAD = 233;

static ReplyBotCooldownEntry replybotCooldown[REPLYBOT_COOLDOWN_SLOTS];
static uint8_t replybotCooldownIdx = 0;

//
// Keyword auto-response table
//
// Each entry maps a keyword to a static text that is sent back when an incoming
// message starts with that keyword (exact command-prefix match: the keyword
// must be followed by end-of-string or whitespace).  Responses are sent on the
// same channel the triggering message arrived on.  Edit this table to add or
// remove your own static keyword/response pairs.  (Dynamic commands like /net
// and /node are handled in buildStatusReply() instead.)
//
struct ReplyBotKeyword {
    const char *keyword;
    const char *response;
};

static constexpr ReplyBotKeyword replybotKeywords[] = {
    {"/help", "Commands: /ping /hello /test /net /node /route /help | /info=/net | bare 'test' 'ping' 'net' 'info' also work"},
};

// Defined below; used by handleReceived() to detect dynamic commands and word triggers.
static bool replybotMatchesPrefix(const char *msg, const char *kw);
static bool replybotEqualsWordCI(const char *msg, const char *word);

// Return true if a reply should be rate-limited for this sender, updating the
// entry table as needed.
static bool replybotRateLimited(uint32_t from, uint32_t cooldownMs)
{
    const uint32_t now = millis();
    for (auto &e : replybotCooldown) {
        if (e.from == from) {
            // Found existing entry; check if cooldown expired
            if ((uint32_t)(now - e.lastMs) < cooldownMs) {
                return true;
            }
            e.lastMs = now;
            return false;
        }
    }
    // No entry found – insert new sender into the ring
    replybotCooldown[replybotCooldownIdx].from = from;
    replybotCooldown[replybotCooldownIdx].lastMs = now;
    replybotCooldownIdx = (replybotCooldownIdx + 1) % REPLYBOT_COOLDOWN_SLOTS;
    return false;
}

// Constructor – registers a single text port and marks the module promiscuous
// so that broadcast messages on any channel are visible.
ReplyBotModule::ReplyBotModule() : SinglePortModule("replybot", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    isPromiscuous = true;
}

void ReplyBotModule::setup()
{
    // In future we may add a protobuf configuration; for now the module is
    // always enabled when compiled in.
}

// Determine whether we want to process this packet.  We only care about
// plain text messages addressed to our port.
bool ReplyBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return (p && p->decoded.portnum == ourPortNum);
}

ProcessMessage ReplyBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Accept direct messages to us, or broadcasts on ANY channel the device is
    // configured for. A received broadcast was already decrypted, so mp.channel
    // is a valid channel index; sendChannelReply() replies back on that same
    // channel. (Previously restricted to the Primary channel only.)
    const uint32_t ourNode = nodeDB->getNodeNum();
    const bool isDM = (mp.to == ourNode);
    const bool isChannelBroadcast = isBroadcast(mp.to);
    if (!isDM && !isChannelBroadcast) {
        return ProcessMessage::CONTINUE;
    }

    // Ignore empty payloads
    if (mp.decoded.payload.size == 0) {
        return ProcessMessage::CONTINUE;
    }

    // Copy payload into a null-terminated buffer
    char buf[260];
    memset(buf, 0, sizeof(buf));
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);

    // Decide what kind of reply (if any) this message warrants:
    //  - mic-check commands (/ping /hello /test): signal report, replied via DM
    //  - status commands (/net /node): live network/device info, computed now
    //  - keyword table (/help ...): static canned response
    const bool isMicCheck = isCommand(buf);
    const bool isWord = replybotEqualsWordCI(buf, "test") || replybotEqualsWordCI(buf, "ping");
    const bool isRoute = replybotMatchesPrefix(buf, "/route");
    const char *keywordResponse = matchKeyword(buf);
    char statusBuf[REPLYBOT_MAX_PAYLOAD];
    const bool isStatus = buildStatusReply(buf, statusBuf, sizeof(statusBuf));
    if (!isMicCheck && !isWord && !isRoute && !keywordResponse && !isStatus) {
        return ProcessMessage::CONTINUE;
    }

    // Apply rate limiting per sender depending on DM/broadcast
    const uint32_t cooldownMs = isDM ? REPLYBOT_DM_COOLDOWN_MS : REPLYBOT_LF_COOLDOWN_MS;
    if (replybotRateLimited(mp.from, cooldownMs)) {
        return ProcessMessage::CONTINUE;
    }

    if (isMicCheck || isWord) {
        // Mic-check: report how well the sender's signal reached us, replied where it was
        // asked (on the channel for a channel broadcast, via DM for a DM).
        int hopsAway = getHopsAway(mp);
        int rssi = mp.rx_rssi;
        if (rssi > 0) {
            rssi -= 200;
        }
        float snr = mp.rx_snr;
        char reply[96];
        snprintf(reply, sizeof(reply), "🎙️ Mic Check : %d Hops away | RSSI %d | SNR %.1f", hopsAway, rssi, snr);
        sendChannelReply(mp, reply);
    } else if (isRoute) {
        // /route: fire an async traceroute toward the sender. The path is delivered
        // later via onTraceRouteResult(). Register the observer lazily here because
        // traceRouteModule is constructed after us in setupModules().
        if (!traceCbRegistered && traceRouteModule) {
            traceRouteModule->setResultObserver([this](uint32_t n, const meshtastic_RouteDiscovery *r, const String &f) {
                this->onTraceRouteResult(n, r, f.c_str());
            });
            traceCbRegistered = true;
        }
        if (!traceRouteModule || tracePending || !traceRouteModule->startTraceRoute(mp.from, REPLYBOT_ROUTE_TIMEOUT_MS)) {
            sendChannelReply(mp, "🛤 trace busy, try again shortly");
        } else {
            tracePending = true;
            traceRequester = mp.from;
            traceChannel = mp.channel;
            traceIsDM = isDM;
            traceMsgId = mp.id;
            LOG_INFO("ReplyBot /route: tracing 0x%0x", mp.from);
            sendChannelReply(mp, "🛤 Trace started — this can take a few seconds. Please wait for the result (or a timeout).");
        }
    } else if (isStatus) {
        // Live network/device status, replied on the same channel the request arrived on.
        LOG_INFO("ReplyBot status cmd from=0x%0x: %.*s", mp.from, mp.decoded.payload.size, mp.decoded.payload.bytes);
        sendChannelReply(mp, statusBuf);
    } else {
        // Static keyword response, replied on the same channel.
        LOG_INFO("ReplyBot keyword matched from=0x%0x: %.*s", mp.from, mp.decoded.payload.size,
                 mp.decoded.payload.bytes);
        sendChannelReply(mp, keywordResponse);
    }
    return ProcessMessage::CONTINUE;
}

// Check if the message starts with one of the supported mic-check commands.
// Leading whitespace is skipped and commands must be followed by end-of-string
// or whitespace.
bool ReplyBotModule::isCommand(const char *msg) const
{
    if (!msg)
        return false;
    while (*msg == ' ' || *msg == '\t')
        msg++;
    auto isEndOrSpace = [](char c) { return c == '\0' || std::isspace(static_cast<unsigned char>(c)); };
    if (strncmp(msg, "/ping", 5) == 0 && isEndOrSpace(msg[5]))
        return true;
    if (strncmp(msg, "/hello", 6) == 0 && isEndOrSpace(msg[6]))
        return true;
    if (strncmp(msg, "/test", 5) == 0 && isEndOrSpace(msg[5]))
        return true;
    return false;
}

// Return true if msg starts with kw as an exact command prefix: leading
// whitespace is skipped and the keyword must be followed by end-of-string or
// whitespace (so "/help" matches "/help" and "/help me" but not "/helpme").
static bool replybotMatchesPrefix(const char *msg, const char *kw)
{
    while (*msg == ' ' || *msg == '\t')
        msg++;
    size_t klen = strlen(kw);
    if (strncmp(msg, kw, klen) != 0)
        return false;
    char next = msg[klen];
    return next == '\0' || std::isspace(static_cast<unsigned char>(next));
}

// True if the entire message equals `word` case-insensitively, ignoring leading/trailing
// whitespace (so "test", " TEST ", "Ping\n" match, but "testing" / "did you test?" do not).
static bool replybotEqualsWordCI(const char *msg, const char *word)
{
    if (!msg || !word)
        return false;
    while (*msg == ' ' || *msg == '\t')
        msg++;
    size_t wlen = strlen(word);
    for (size_t i = 0; i < wlen; i++)
        if (std::tolower((unsigned char)msg[i]) != std::tolower((unsigned char)word[i]))
            return false;
    const char *p = msg + wlen;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return *p == '\0';
}

// Look up the message against the keyword table, returning the mapped response
// string for the first matching keyword, or nullptr if none match.
const char *ReplyBotModule::matchKeyword(const char *msg) const
{
    if (!msg)
        return nullptr;
    for (const auto &k : replybotKeywords)
        if (replybotMatchesPrefix(msg, k.keyword))
            return k.response;
    return nullptr;
}

// Build a live status reply for /net and /node.  Returns true and fills `out`
// when `msg` is one of those commands, false otherwise.  Values are read from
// the same globals DeviceTelemetry uses.
// /net also answers to the aliases /info and the bare whole-words "net" / "info".
bool ReplyBotModule::buildStatusReply(const char *msg, char *out, size_t outLen)
{
    if (replybotMatchesPrefix(msg, "/net") || replybotMatchesPrefix(msg, "/info") ||
        replybotEqualsWordCI(msg, "net") || replybotEqualsWordCI(msg, "info")) {
        unsigned online = (unsigned)nodeDB->getNumOnlineMeshNodes();
        unsigned total = (unsigned)nodeDB->getNumMeshNodes();
        float ch = airTime ? airTime->channelUtilizationPercent() : 0.0f;
        float tx = airTime ? airTime->utilizationTXPercent() : 0.0f;
        char noise[24] = "";
        if (RadioLibInterface::instance) {
            RadioLibInterface::instance->updateNoiseFloor();
            snprintf(noise, sizeof(noise), " | noise %ddBm", (int)RadioLibInterface::instance->getAverageNoiseFloor());
        }
        snprintf(out, outLen, "📡 %u/%u nodes | ChUtil %.0f%% | TxAir %.0f%%%s", online, total, ch, tx, noise);
        return true;
    }
    if (replybotMatchesPrefix(msg, "/node")) {
        char batt[24];
        if (powerStatus && powerStatus->getHasBattery())
            snprintf(batt, sizeof(batt), "🔋%u%%%s", (unsigned)powerStatus->getBatteryChargePercent(),
                     powerStatus->getIsCharging() ? "⚡" : "");
        else
            snprintf(batt, sizeof(batt), "USB");
        uint32_t s = millis() / 1000;
        uint32_t h = s / 3600;
        uint32_t m = (s % 3600) / 60;
        char up[24];
        if (h >= 24)
            snprintf(up, sizeof(up), "%ud%uh", h / 24, h % 24);
        else
            snprintf(up, sizeof(up), "%uh%um", h, m);
        snprintf(out, outLen, "🔧 FW%s %s | %s | up %s", optstr(APP_VERSION_SHORT), REPLYBOT_MOD_TAG, batt, up);
        return true;
    }
    return false;
}

// Send a reply on the same channel the triggering message arrived on.  For a
// broadcast trigger the response is broadcast back on that channel so everyone
// sees it; for a direct message the response is sent privately to the sender.
void ReplyBotModule::sendChannelReply(const meshtastic_MeshPacket &rx, const char *text)
{
    if (!text)
        return;
    meshtastic_MeshPacket *p = allocDataPacket();
    const bool isDM = (rx.to == nodeDB->getNodeNum());
    p->to = isDM ? rx.from : NODENUM_BROADCAST;
    p->channel = rx.channel;
    p->decoded.reply_id = rx.id; // render as a reply to the original command message
    p->want_ack = false;
    p->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > sizeof(p->decoded.payload.bytes)) {
        len = sizeof(p->decoded.payload.bytes);
    }
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    service->sendToMesh(p);
}

// Writes "long_name [short_name] 0xID" (when the node is named) or just "0xID" into `label`.
// Mirrors TraceRouteModule::getNodeName's field access.
static void replybotNodeLabel(char *label, size_t cap, uint32_t node)
{
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    bool named = info && nodeInfoLiteHasUser(info) && (info->long_name[0] || info->short_name[0]);
    if (named && info->long_name[0] && info->short_name[0])
        snprintf(label, cap, "%s [%s] 0x%08x", info->long_name, info->short_name, node);
    else if (named)
        snprintf(label, cap, "%s 0x%08x", info->long_name[0] ? info->long_name : info->short_name, node);
    else
        snprintf(label, cap, "0x%08x", node);
}

// Emits one node line ("<label>[ 📶 X.XdB]\n"), preceded by a "⬇️\n" line unless it's the first.
static void replybotEmitNode(char *buf, size_t cap, size_t *off, uint32_t node, bool hasSnr, float snr, bool *first)
{
    if (*off >= cap - 1)
        return;
    if (!*first) {
        int w = snprintf(buf + *off, cap - *off, "⬇️\n");
        if (w > 0)
            *off += (size_t)w;
        if (*off >= cap)
            *off = cap - 1;
    }
    *first = false;
    if (*off >= cap - 1)
        return;
    char label[64];
    replybotNodeLabel(label, sizeof(label), node);
    int w = hasSnr ? snprintf(buf + *off, cap - *off, "%s 📶 %.1fdB\n", label, snr)
                   : snprintf(buf + *off, cap - *off, "%s\n", label);
    if (w > 0)
        *off += (size_t)w;
    if (*off >= cap)
        *off = cap - 1;
}

// Builds one direction's node chain into `buf`. towards=true => me->...->dest; false => dest->...->me.
// Node ordering + SNR pairing mirror TraceRouteModule's result formatting.
static void replybotBuildPath(char *buf, size_t cap, const meshtastic_RouteDiscovery *r, uint32_t selfNode,
                              uint32_t destNode, bool towards)
{
    buf[0] = '\0';
    size_t off = 0;
    bool first = true;
    const uint32_t *route = towards ? r->route : r->route_back;
    int routeCount = towards ? (int)r->route_count : (int)r->route_back_count;
    const int8_t *snr = towards ? r->snr_towards : r->snr_back;
    int snrCount = towards ? (int)r->snr_towards_count : (int)r->snr_back_count;

    auto snrAt = [&](int i, bool &has) -> float {
        if (i >= 0 && i < snrCount && snr[i] != INT8_MIN) {
            has = true;
            return snr[i] / 4.0f;
        }
        has = false;
        return 0.0f;
    };
    bool has;
    float v;

    if (towards) {
        replybotEmitNode(buf, cap, &off, selfNode, false, 0, &first); // initiator, no SNR
        for (int i = 0; i < routeCount; i++) {
            v = snrAt(i, has);
            replybotEmitNode(buf, cap, &off, route[i], has, v, &first);
        }
        v = snrAt(snrCount - 1, has); // destination uses the last SNR
        replybotEmitNode(buf, cap, &off, destNode, has, v, &first);
    } else {
        replybotEmitNode(buf, cap, &off, destNode, false, 0, &first); // destination, no SNR
        for (int i = routeCount - 1; i >= 0; i--) {
            v = snrAt(i, has);
            replybotEmitNode(buf, cap, &off, route[i], has, v, &first);
        }
        v = snrAt(snrCount - 1, has); // back to us, last SNR
        replybotEmitNode(buf, cap, &off, selfNode, has, v, &first);
    }
}

// Called by TraceRouteModule (via the observer we registered) when a trace we started completes or
// times out. Formats the node-by-node path and replies to the original requester (threaded), as one
// message — or two (out path, then back path) when it would exceed the 233-byte payload.
void ReplyBotModule::onTraceRouteResult(uint32_t tracedNode, const meshtastic_RouteDiscovery *route, const char *fallback)
{
    if (!tracePending || tracedNode != traceRequester)
        return; // not the trace we requested
    tracePending = false;

    // Synthesize an rx so sendChannelReply targets the requester/channel and threads to the /route msg.
    meshtastic_MeshPacket rx = {};
    rx.from = traceRequester;
    rx.to = traceIsDM ? nodeDB->getNodeNum() : NODENUM_BROADCAST;
    rx.channel = traceChannel;
    rx.id = traceMsgId;

    if (!route) { // timeout / no result
        char out[64];
        snprintf(out, sizeof(out), "🛤 %s", fallback ? fallback : "No route");
        sendChannelReply(rx, out);
        return;
    }

    const uint32_t selfNode = nodeDB->getNodeNum();
    char destLabel[64];
    replybotNodeLabel(destLabel, sizeof(destLabel), traceRequester);
    char outBlock[REPLYBOT_MAX_PAYLOAD], backBlock[REPLYBOT_MAX_PAYLOAD];
    replybotBuildPath(outBlock, sizeof(outBlock), route, selfNode, traceRequester, true);
    replybotBuildPath(backBlock, sizeof(backBlock), route, selfNode, traceRequester, false);

    char combined[REPLYBOT_MAX_PAYLOAD];
    int n = snprintf(combined, sizeof(combined), "🛤 Route ⇄ %s\n%s\n%s", destLabel, outBlock, backBlock);
    if (n >= 0 && n < (int)sizeof(combined)) {
        sendChannelReply(rx, combined); // fits in one message
    } else {                            // too long: split into out + back
        char m[REPLYBOT_MAX_PAYLOAD];
        snprintf(m, sizeof(m), "🛤 Route ⇄ %s\n%s", destLabel, outBlock);
        sendChannelReply(rx, m);
        snprintf(m, sizeof(m), "🛤 Route ⇄ %s\n%s", destLabel, backBlock);
        sendChannelReply(rx, m);
    }
}
#endif // MESHTASTIC_EXCLUDE_REPLYBOT
