//
//  AgentConnection.hpp
//  Macchina — new-cpp/core/client
//
//  The v3 connect/subscribe/establish flow, platform-agnostic: everything
//  goes through the abstract Transport. Port of new/core/NIAgentConnection.
//
//  Flow (decoded live — new/docs/protocol.md "Protocol v3 handshake"):
//    GetServiceVersion  0xNN536756        → 8-byte v3 reply {0x00020802, 3}
//    DeviceSubscribe    0xNN447500 "Du"   [product][clientTag][role][0]
//    DeviceConnect      0xNN444900        [product][clientTag][role][len][serial\0]
//    SetAsciiString     0xNN404300        register our notification port
//    Subscribe void-op  0xNN447143        real agent replies empty
//  where NN is the negotiated protocol version (0x02 → 0x03).
//
//  The reply to the FIRST inbound frame on a fresh receive port (the
//  MsgConnectionEstablished completion, tag undecoded) is switchable at
//  runtime:  MACCHINA_ESTABLISH_REPLY = echo (default) | true | empty | none
//

#pragma once

#include "core/protocol/Protocol.hpp"
#include "core/transport/Transport.hpp"

#include <memory>

namespace macchina::nhl2 {

/// One established connection: the agent-assigned request port we send to,
/// and the receive port we own, on which the agent's completion handshake and
/// later notifications (input events) arrive.
class Endpoints
{
public:
    std::string                  label;
    std::unique_ptr<RequestPort> request;

    /// Called for every inbound frame AFTER the initial
    /// MsgConnectionEstablished handshake frame — i.e. event notifications.
    /// Set by a higher layer (the Controller). Events are fire-and-forget.
    std::function<void(const Frame &)> onInboundFrame;

    ReceivePort & notifications() const { return *notifications_; }
    void setQuiet(bool quiet);

private:
    friend class AgentConnection;
    bool seenFirstFrame_ = false;

    // Declared last → destroyed first, so the receive port (whose handler
    // points back into this object) is torn down before anything else.
    std::unique_ptr<ReceivePort> notifications_;
};


class AgentConnection
{
public:
    /// nullptr if the service's main port doesn't exist (agent not running).
    static std::unique_ptr<AgentConnection> open(Transport & transport,
                                                 const std::string & mainPortName);

    uint32_t versionPrefix() const { return versionPrefix_; }
    RequestPort & mainPort() { return *mainPort_; }

    /// (versionPrefix << 24) | tag — unless tag already uses all 4 bytes
    /// (full-4CC messages like "SMap"), which pass through unchanged.
    uint32_t messageIdFor(uint32_t tag) const;

    std::optional<Frame> sendTag(uint32_t tag, const Frame & body, RequestPort & port);
    std::optional<Frame> sendTag(uint32_t tag, const Frame & body);   // on mainPort

    /// GetServiceVersion. Any usable reply upgrades us to the v3 prefix
    /// (both daemons want 0x03 regardless of reply length — LEARNINGS.md E13).
    bool negotiateVersion();

    /// "Du" — subscription connection (device add/remove notifications).
    PortPairReply subscribe(uint32_t productId, uint32_t clientTag, uint32_t role);

    /// DeviceConnect — controller connection, trailing ASCII serial + NUL.
    PortPairReply connectDevice(uint32_t productId, uint32_t clientTag, uint32_t role,
                                const std::string & serial);

    /// Stand up both ports for a successful connect reply; the receive port
    /// starts delivering on this thread's event loop.
    std::unique_ptr<Endpoints> openEndpoints(const PortPairReply & reply,
                                             const std::string & label);

    /// SetAsciiString(our notification-port name) on the connection's request
    /// port — the v3 "register" step ([8 zero bytes][len][name+NUL]).
    std::optional<Frame> registerNotificationPort(Endpoints & endpoints);
    std::optional<Frame> registerNotificationPortName(const std::string & portName,
                                                      RequestPort & requestPort);

    /// 0x447143 — the v3 "subscribe" void-op; the real agent replies empty.
    std::optional<Frame> sendSubscribeVoidOp(Endpoints & endpoints);

private:
    AgentConnection(Transport & transport, std::unique_ptr<RequestPort> mainPort);

    PortPairReply connectWithIdString(uint32_t tag, uint32_t productId, uint32_t clientTag,
                                      uint32_t role, const std::vector<uint8_t> & idString,
                                      uint32_t terminatorSize);

    Transport &                  transport_;
    std::unique_ptr<RequestPort> mainPort_;
    uint32_t                     versionPrefix_ = 0x02;
};

} // namespace macchina::nhl2
