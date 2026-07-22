//
//  AgentConnection.cpp
//  Macchina — new-cpp/core/client
//

#include "core/client/AgentConnection.hpp"
#include "core/transport/HexDump.hpp"

#include <cstdio>
#include <cstdlib>

namespace macchina::nhl2 {

// Reply to the MsgConnectionEstablished candidate frame, per the
// MACCHINA_ESTABLISH_REPLY research switch (see header).
static std::optional<Frame> establishReply(const std::string & strategy, const Frame & frame)
{
    if (strategy == "none")
        return std::nullopt;
    if (strategy == "empty")
        return Frame{};
    if (strategy == "true")
    {
        Frame t;
        appendU32(t, kReplyTrue);
        return t;
    }
    return frame;   // echo (default)
}


void Endpoints::setQuiet(bool quiet)
{
    if (request)
        request->quiet = quiet;
    if (notifications_)
        notifications_->quiet = quiet;
}


AgentConnection::AgentConnection(Transport & transport, std::unique_ptr<RequestPort> mainPort)
    : transport_(transport), mainPort_(std::move(mainPort))
{
}

std::unique_ptr<AgentConnection> AgentConnection::open(Transport & transport,
                                                       const std::string & mainPortName)
{
    auto mainPort = transport.openRequestPort(mainPortName);
    if (mainPort == nullptr)
        return nullptr;
    return std::unique_ptr<AgentConnection>(
        new AgentConnection(transport, std::move(mainPort)));
}

uint32_t AgentConnection::messageIdFor(uint32_t tag) const
{
    if (tag & 0xff000000)   // full 4CC message ("SMap", "APP ", …)
        return tag;
    return (versionPrefix_ << 24) | tag;
}

std::optional<Frame> AgentConnection::sendTag(uint32_t tag, const Frame & body,
                                              RequestPort & port)
{
    Frame frame;
    frame.reserve(4 + body.size());
    appendU32(frame, messageIdFor(tag));
    frame.insert(frame.end(), body.begin(), body.end());
    return port.request(frame);
}

std::optional<Frame> AgentConnection::sendTag(uint32_t tag, const Frame & body)
{
    return sendTag(tag, body, *mainPort_);
}

bool AgentConnection::negotiateVersion()
{
    auto reply = sendTag(kTagGetServiceVersion, {});
    if (!reply)
    {
        fprintf(stderr, "    no GetServiceVersion reply\n");
        return false;
    }

    if (reply->size() >= 8)
    {
        versionPrefix_ = 0x03;
        fprintf(stderr, "    service version 0x%08x, count %u — switching to v3 prefix\n",
                *readU32(*reply, 0), *readU32(*reply, 4));
        return true;
    }
    if (reply->size() == 4)
    {
        // A 4-byte version reply still means "speak v3": both daemons want the
        // 0x03 prefix regardless (M2 capture, LEARNINGS.md E13 — HIA answers
        // 0x00010f00 yet M2 still sends 0x03447500).
        versionPrefix_ = 0x03;
        fprintf(stderr, "    4-byte version 0x%08x — using 0x03 prefix (both daemons, per M2 capture)\n",
                *readU32(*reply, 0));
        return true;
    }

    fprintf(stderr, "    no usable GetServiceVersion reply (%zu bytes)\n", reply->size());
    return false;
}

PortPairReply AgentConnection::subscribe(uint32_t productId, uint32_t clientTag, uint32_t role)
{
    Frame body;
    appendU32(body, productId);
    appendU32(body, clientTag);
    appendU32(body, role);
    appendU32(body, 0);

    auto reply = sendTag(kTagDeviceSubscribe, body);
    return reply ? PortPairReply::parse(*reply) : PortPairReply{};
}

// Shared tail of the DeviceConnect variants: [len][bytes][NUL(s)]. The length
// field counts the terminator too (live capture: len=9 for "39195855").
PortPairReply AgentConnection::connectWithIdString(uint32_t tag, uint32_t productId,
                                                   uint32_t clientTag, uint32_t role,
                                                   const std::vector<uint8_t> & idString,
                                                   uint32_t terminatorSize)
{
    Frame body;
    appendU32(body, productId);
    appendU32(body, clientTag);
    appendU32(body, role);
    appendU32(body, (uint32_t)idString.size() + terminatorSize);
    body.insert(body.end(), idString.begin(), idString.end());
    body.insert(body.end(), terminatorSize, 0);

    auto reply = sendTag(tag, body);
    return reply ? PortPairReply::parse(*reply) : PortPairReply{};
}

PortPairReply AgentConnection::connectDevice(uint32_t productId, uint32_t clientTag,
                                             uint32_t role, const std::string & serial)
{
    std::vector<uint8_t> bytes(serial.begin(), serial.end());
    return connectWithIdString(kTagDeviceConnect, productId, clientTag, role, bytes, 1);
}

std::unique_ptr<Endpoints> AgentConnection::openEndpoints(const PortPairReply & reply,
                                                          const std::string & label)
{
    if (!reply.success)
    {
        fprintf(stderr, "[%s] connect refused — no endpoints to open\n", label.c_str());
        return nullptr;
    }

    auto request = transport_.openRequestPort(reply.requestPortName);
    if (request == nullptr)
    {
        fprintf(stderr, "[%s] agent assigned request port '%s' but it doesn't exist\n",
                label.c_str(), reply.requestPortName.c_str());
        return nullptr;
    }

    auto endpoints = std::make_unique<Endpoints>();
    endpoints->label   = label;
    endpoints->request = std::move(request);

    const char * strategyEnv = getenv("MACCHINA_ESTABLISH_REPLY");
    std::string  strategy    = strategyEnv ? strategyEnv : "echo";

    // The first frame the agent sends here should be the connection
    // completion (MsgConnectionEstablished, tag undecoded); later frames are
    // event notifications, handed to onInboundFrame with no reply. The
    // handler holds a raw pointer to the Endpoints object; safe because the
    // receive port is a member destroyed first (see Endpoints).
    Endpoints * ep = endpoints.get();
    FrameHandler handler = [ep, strategy, label](const Frame & frame) -> std::optional<Frame>
    {
        if (ep->seenFirstFrame_)
        {
            if (ep->onInboundFrame)
                ep->onInboundFrame(frame);
            return std::nullopt;
        }
        ep->seenFirstFrame_ = true;

        fprintf(stderr, "*** [%s] FIRST inbound frame — MsgConnectionEstablished candidate, "
                "replying per strategy '%s'\n", label.c_str(), strategy.c_str());
        return establishReply(strategy, frame);
    };

    endpoints->notifications_ =
        transport_.createReceivePort(reply.notificationPortName, std::move(handler));
    if (endpoints->notifications_ == nullptr)
    {
        fprintf(stderr, "[%s] could not create receive port '%s' — name taken "
                "(stale port from a dead client?)\n",
                label.c_str(), reply.notificationPortName.c_str());
        return nullptr;
    }

    fprintf(stderr, "[%s] endpoints open — request '%s', receive '%s'\n",
            label.c_str(), reply.requestPortName.c_str(), reply.notificationPortName.c_str());
    return endpoints;
}

std::optional<Frame> AgentConnection::registerNotificationPort(Endpoints & endpoints)
{
    return registerNotificationPortName(endpoints.notifications().name(), *endpoints.request);
}

std::optional<Frame> AgentConnection::registerNotificationPortName(const std::string & portName,
                                                                   RequestPort & requestPort)
{
    Frame body;
    // Decoded from Maschine 2's NHL2::MessageSetString ctor (LEARNINGS.md E11):
    // the register frame is [msgid][8 ZERO bytes][strlen][name+NUL] — the ctor
    // never writes those 8 bytes, so they go out zero.
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, (uint32_t)portName.size() + 1);
    body.insert(body.end(), portName.begin(), portName.end());
    body.push_back(0);

    return sendTag(kTagSetAsciiString, body, requestPort);
}

std::optional<Frame> AgentConnection::sendSubscribeVoidOp(Endpoints & endpoints)
{
    return sendTag(kTagSubscribeVoidOp, {}, *endpoints.request);
}

} // namespace macchina::nhl2
