#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
#include "SinglePortModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

class ReplyBotModule : public SinglePortModule
{
  public:
    ReplyBotModule();
    void setup() override;
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  protected:
    bool isCommand(const char *msg) const;
    const char *matchKeyword(const char *msg) const;                  // returns response string or nullptr
    bool buildStatusReply(const char *msg, char *out, size_t outLen); // dynamic /net, /node
    void sendChannelReply(const meshtastic_MeshPacket &rx, const char *text);
    void onTraceRouteResult(uint32_t tracedNode, const meshtastic_RouteDiscovery *route,
                            const char *fallback); // called via TraceRouteModule observer

    // Single in-flight /route request (traceroute runs one at a time).
    bool traceCbRegistered = false;
    bool tracePending = false;
    uint32_t traceRequester = 0; // node to reply to
    uint8_t traceChannel = 0;
    bool traceIsDM = false;
    uint32_t traceMsgId = 0; // id of the /route message, for reply_id on the async result
};
#endif // MESHTASTIC_EXCLUDE_REPLYBOT