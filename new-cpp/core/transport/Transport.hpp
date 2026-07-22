//
//  Transport.hpp
//  Macchina — new-cpp/core/transport
//
//  The abstract message transport. This is the ONLY interface the protocol
//  client depends on; nothing here may mention CFMessagePort, run loops, or
//  any other platform concept. The macOS implementation lives in
//  core/transport-macos/.
//
//  Model (mirrors what the NHL2 agents actually do):
//    - RequestPort  — a channel to a named peer endpoint; synchronous
//                     request/reply plus fire-and-forget post.
//    - ReceivePort  — a named local endpoint the peer pushes frames to;
//                     frames are delivered to a handler on the event-loop
//                     thread, which may answer with a reply frame.
//    - Transport    — the factory for both, plus the event loop that
//                     dispatches inbound frames.
//

#pragma once

#include "core/Frame.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace macchina {

class RequestPort
{
public:
    virtual ~RequestPort() = default;

    /// Send and block for the peer's reply.
    /// nullopt = transport error (timeout, dead port); an *empty* Frame is a
    /// valid empty reply and means the peer accepted (set-style messages).
    virtual std::optional<Frame> request(const Frame & frame) = 0;

    /// Fire-and-forget send; no reply is waited for.
    virtual void post(const Frame & frame) = 0;

    virtual const std::string & name() const = 0;

    /// Suppress per-frame logging (bulk paths: draws, LED streams).
    bool quiet = false;
};

/// Handler for inbound frames. Runs on the event-loop thread.
/// Return a Frame to reply (empty Frame = empty reply); nullopt = no reply.
using FrameHandler = std::function<std::optional<Frame>(const Frame & frame)>;

class ReceivePort
{
public:
    virtual ~ReceivePort() = default;

    virtual const std::string & name() const = 0;

    bool quiet = false;
};

class Transport
{
public:
    virtual ~Transport() = default;

    /// Open a channel to the peer endpoint with this name.
    /// nullptr if no peer owns the name (e.g. the agent isn't running).
    virtual std::unique_ptr<RequestPort> openRequestPort(const std::string & name) = 0;

    /// Create a local endpoint and start delivering the peer's frames to
    /// `handler` (delivery happens while run()/runFor() executes).
    /// nullptr if the name is already taken (stale port from a dead client).
    virtual std::unique_ptr<ReceivePort> createReceivePort(const std::string & name,
                                                           FrameHandler handler) = 0;

    /// The event loop: block and dispatch inbound frames.
    virtual void run() = 0;

    /// Dispatch inbound frames for (at most) `seconds`, then return.
    virtual void runFor(double seconds) = 0;
};

} // namespace macchina
